/* apt-intf.cpp
 *
 * Copyright (c) 1999-2008 Daniel Burrows
 * Copyright (c) 2004 Michael Vogt <mvo@debian.org>
 *               2009-2018 Daniel Nicoletti <dantti12@gmail.com>
 *               2012-2022 Matthias Klumpp <matthias@tenstral.net>
 *               2016 Harald Sitter <sitter@kde.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "apt-intf.h"

#include <apt-pkg/init.h>
#include <apt-pkg/error.h>
#include <apt-pkg/fileutl.h>
#include <apt-pkg/sourcelist.h>
#include <apt-pkg/update.h>
#include <apt-pkg/algorithms.h>
#include <apt-pkg/pkgsystem.h>
#include <apt-pkg/version.h>

#ifdef WITH_LUA
#include <apt-pkg/luaiface.h>
#endif

#include <appstream.h>

#include <sys/prctl.h>
#include <sys/statvfs.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <sys/fcntl.h>
#include <pty.h>

#include <algorithm>
#include <iostream>
#include <sstream>
#include <memory>
#include <fstream>
#include <dirent.h>
#include <regex.h>

#include "apt-cache-file.h"
#include "apt-utils.h"
#include "gst-matcher.h"
#include "apt-messages.h"
#include "acqpkitstatus.h"

#define RAMFS_MAGIC     0x858458f6

AptIntf::AptIntf(PkBackendJob *job) :
    m_job(job),
    m_cancel(false),
    m_lastSubProgress(0),
    m_terminalTimeout(120),
    m_progress(OpPackageKitProgress(job))
{
}

bool AptIntf::init(gchar **localDebs)
{
    const gchar *http_proxy;
    const gchar *ftp_proxy;

    // set locale
    setEnvLocaleFromJob();

    // set http proxy
    http_proxy = pk_backend_job_get_proxy_http(m_job);
    if (http_proxy != NULL) {
        g_autofree gchar *uri = pk_backend_convert_uri(http_proxy);
        g_setenv("http_proxy", uri, TRUE);
    }

    // set ftp proxy
    ftp_proxy = pk_backend_job_get_proxy_ftp(m_job);
    if (ftp_proxy != NULL) {
        g_autofree gchar *uri = pk_backend_convert_uri(ftp_proxy);
        g_setenv("ftp_proxy", uri, TRUE);
    }

    // Check if we should open the Cache with lock
    bool withLock = false;
    bool AllowBroken = false;
    PkRoleEnum role = pk_backend_job_get_role(m_job);
    switch (role) {
    case PK_ROLE_ENUM_INSTALL_PACKAGES:
    case PK_ROLE_ENUM_INSTALL_FILES:
    case PK_ROLE_ENUM_REMOVE_PACKAGES:
    case PK_ROLE_ENUM_UPDATE_PACKAGES:
        withLock = true;
        break;
    case PK_ROLE_ENUM_REPAIR_SYSTEM:
        AllowBroken = true;
        break;
    default:
        withLock = false;
    }

    bool simulate = false;
    if (withLock) {
        // Get the simulate value to see if the lock is valid
        PkBitfield transactionFlags = pk_backend_job_get_transaction_flags(m_job);
        simulate = pk_bitfield_contain(transactionFlags, PK_TRANSACTION_FLAG_ENUM_SIMULATE);

        // Disable the lock if we are simulating
        withLock = !simulate;
    }

    int timeout = 10;
    // TODO test this
    if (withLock) {
        for (;;) {
            m_fileFd.Fd(GetLock(_config->FindDir("Dir::Cache::Archives") + "lock"));
            if (not _error->PendingError())
            {
                break;
            }

            if (timeout <= 0)
            {
                show_errors(m_job, PK_ERROR_ENUM_CANNOT_GET_LOCK);
                return false;
            }

            _error->Discard();
            pk_backend_job_set_status(m_job, PK_STATUS_ENUM_WAITING_FOR_LOCK);
            sleep(1);
            timeout--;
        }
    }

    // Create the AptCacheFile class to search for packages
    m_cache.reset(new AptCacheFile(m_job, withLock, &m_progress));
    while (m_cache->Open() == false) {
        if (withLock == false || (timeout <= 0)) {
            show_errors(m_job, PK_ERROR_ENUM_CANNOT_GET_LOCK);
            return false;
        } else {
            _error->Discard();
            pk_backend_job_set_status(m_job, PK_STATUS_ENUM_WAITING_FOR_LOCK);
            sleep(1);
            timeout--;
        }

        // If we are going to try again, we can either simply try Open() once
        // again (since pkgCacheFile is monotonic in creating required objects),
        // or simply continue with a new pkgCacheFile object.
        m_cache.reset(new AptCacheFile(m_job, withLock, &m_progress));
    }

    // default settings
    _config->CndSet("APT::Get::AutomaticRemove::Kernels", _config->FindB("APT::Get::AutomaticRemove", true));

    m_interactive = pk_backend_job_get_interactive(m_job);
    if (!m_interactive) {
        // Do not ask about config updates if we are not interactive
        _config->Set("Dpkg::Options::", "--force-confdef");
        _config->Set("Dpkg::Options::", "--force-confold");
        // Ensure nothing interferes with questions
        g_setenv("APT_LISTCHANGES_FRONTEND", "none", TRUE);
        g_setenv("APT_LISTBUGS_FRONTEND", "none", TRUE);
    }

    // Check if there are half-installed packages and if we can fix them
    return m_cache->CheckDeps(AllowBroken);
}

void AptIntf::setEnvLocaleFromJob()
{
    const gchar *locale = pk_backend_job_get_locale(m_job);
    if (locale == NULL)
        return;

    // set daemon locale
    setlocale(LC_ALL, locale);

    // processes spawned by APT need to inherit the right locale as well
    g_setenv("LANG", locale, TRUE);
    g_setenv("LANGUAGE", locale, TRUE);
}

void AptIntf::cancel()
{
    if (!m_cancel) {
        m_cancel = true;
        pk_backend_job_set_status(m_job, PK_STATUS_ENUM_CANCEL);
    }

    if (m_child_pid > 0) {
        kill(m_child_pid, SIGTERM);
    }
}

bool AptIntf::cancelled() const
{
    return m_cancel;
}

bool AptIntf::matchPackage(const pkgCache::VerIterator &ver, PkBitfield filters)
{
    if (filters != 0) {
        const pkgCache::PkgIterator &pkg = ver.ParentPkg();
        bool installed = false;

        // Check if the package is installed
        if (pkg->CurrentState == pkgCache::State::Installed && pkg.CurrentVer() == ver)
            installed = true;

        std::string str = ver.Section() == NULL ? "" : ver.Section();
        std::string section, component;

        size_t found;
        found = str.find_last_of("/");
        section = str.substr(found + 1);
        if(found == str.npos) {
            component = "main";
        } else {
            component = str.substr(0, found);
        }

        if (pk_bitfield_contain(filters, PK_FILTER_ENUM_NOT_INSTALLED) && installed)
            return false;
        else if (pk_bitfield_contain(filters, PK_FILTER_ENUM_INSTALLED) && !installed)
            return false;

        if (pk_bitfield_contain(filters, PK_FILTER_ENUM_DEVELOPMENT)) {
            // if ver.end() means unknow
            // strcmp will be true when it's different than devel
            std::string pkgName = pkg.Name();
            if (!ends_with(pkgName, "-dev") &&
                    !ends_with(pkgName, "-dbg") &&
                    section.compare("devel") &&
                    section.compare("libdevel")) {
                return false;
            }
        } else if (pk_bitfield_contain(filters, PK_FILTER_ENUM_NOT_DEVELOPMENT)) {
            std::string pkgName = pkg.Name();
            if (ends_with(pkgName, "-dev") ||
                    ends_with(pkgName, "-dbg") ||
                    !section.compare("devel") ||
                    !section.compare("libdevel")) {
                return false;
            }
        }

        if (pk_bitfield_contain(filters, PK_FILTER_ENUM_GUI)) {
            // if ver.end() means unknow
            // strcmp will be true when it's different than x11
            if (section.compare("x11") && section.compare("gnome") &&
                    section.compare("kde") && section.compare("graphics")) {
                return false;
            }
        } else if (pk_bitfield_contain(filters, PK_FILTER_ENUM_NOT_GUI)) {
            if (!section.compare("x11") || !section.compare("gnome") ||
                    !section.compare("kde") || !section.compare("graphics")) {
                return false;
            }
        }

        if (pk_bitfield_contain(filters, PK_FILTER_ENUM_FREE)) {
            if (component.compare("main") != 0 &&
                    component.compare("universe") != 0) {
                // Must be in main and universe to be free
                return false;
            }
        } else if (pk_bitfield_contain(filters, PK_FILTER_ENUM_NOT_FREE)) {
            if (component.compare("main") == 0 ||
                    component.compare("universe") == 0) {
                // Must not be in main or universe to be free
                return false;
            }
        }

        // TODO test this one..
#if 0
        // I couldn'tfind any packages with the metapackages component, and I
        // think the check is the wrong way around; PK_FILTER_ENUM_COLLECTIONS
        // is for virtual group packages -- hughsie
        if (pk_bitfield_contain(filters, PK_FILTER_ENUM_COLLECTIONS)) {
            if (!component.compare("metapackages")) {
                return false;
            }
        } else if (pk_bitfield_contain(filters, PK_FILTER_ENUM_NOT_COLLECTIONS)) {
            if (component.compare("metapackages")) {
                return false;
            }
        }
#endif
    }
    return true;
}

PkgList AptIntf::filterPackages(const PkgList &packages, PkBitfield filters)
{
    if (filters == 0)
        return packages;

    PkgList ret;
    ret.reserve(packages.size());

    for (const PkgInfo &info : packages) {
        if (matchPackage(info.ver, filters)) {
            ret.push_back(info);
        }
    }

    // This filter is more complex so we filter it after the list has shrunk
    if (pk_bitfield_contain(filters, PK_FILTER_ENUM_DOWNLOADED) && ret.size() > 0) {
        PkgList downloaded;

        pkgProblemResolver Fix(*m_cache);
        {
            for (auto autoInst : { true, false }) {
                for (const PkgInfo &pki : ret) {
                    if (m_cancel)
                        break;

                    m_cache->tryToInstall(Fix, pki, autoInst, false);
                }
            }
        }

        // get a fetcher
        pkgAcquire fetcher;

        pkgSourceList List;
        // Read the source list
        if (List.ReadMainList() == false) {
            return downloaded;
        }

        // Create the package manager and prepare to download
        std::unique_ptr<pkgPackageManager> PM (_system->CreatePM(*m_cache));
        if (!PM->GetArchives(&fetcher, &List, m_cache->GetPkgRecords()) ||
                _error->PendingError() == true) {
            return downloaded;
        }

        for (const PkgInfo &info : ret) {
            bool found = false;
            for (pkgAcquire::ItemIterator it = fetcher.ItemsBegin(); it < fetcher.ItemsEnd(); ++it) {
                pkgAcqArchiveSane *archive = static_cast<pkgAcqArchiveSane*>(dynamic_cast<pkgAcqArchive*>(*it));
                if (archive == nullptr) {
                    continue;
                }
                const pkgCache::VerIterator ver = archive->version();
                if ((*it)->Local && info.ver == ver) {
                    found = true;
                    break;
                }
            }

            if (found)
                downloaded.append(info);
        }

        return downloaded;
    }

    return ret;
}

// used to emit packages it collects all the needed info
void AptIntf::emitPackage(const pkgCache::VerIterator &ver, PkInfoEnum state)
{
    // check the state enum to see if it was not set.
    if (state == PK_INFO_ENUM_UNKNOWN) {
        const pkgCache::PkgIterator &pkg = ver.ParentPkg();

        if (pkg->CurrentState == pkgCache::State::Installed &&
                pkg.CurrentVer() == ver) {
            state = PK_INFO_ENUM_INSTALLED;
        } else {
            state = PK_INFO_ENUM_AVAILABLE;
        }
    }

    g_autofree gchar *package_id = m_cache->buildPackageId(ver);
    pk_backend_job_package(m_job,
                           state,
                           package_id,
                           m_cache->getShortDescription(ver).c_str());
}

void AptIntf::emitPackageProgress(const pkgCache::VerIterator &ver, PkStatusEnum status, uint percentage)
{
    g_autofree gchar *package_id = m_cache->buildPackageId(ver);
    pk_backend_job_set_item_progress(m_job, package_id, status, percentage);
}

void AptIntf::emitPackages(PkgList &output, PkBitfield filters, PkInfoEnum state, bool multiversion)
{
    // Sort so we can remove the duplicated entries
    output.sort();

    // Remove the duplicated entries
    output.removeDuplicates();

    output = filterPackages(output, filters);
    for (const PkgInfo &info : output) {
        if (m_cancel)
            break;

        auto ver = info.ver;
        // emit only the latest/chosen version if newest is requested
        if (!multiversion || pk_bitfield_contain(filters, PK_FILTER_ENUM_NEWEST)) {
            emitPackage(info.ver, state);
            continue;
        } else if (pk_bitfield_contain(filters, PK_FILTER_ENUM_NOT_NEWEST) && !ver.end()) {
            ver++;
        }

        for (; !ver.end(); ver++) {
            emitPackage(ver, state);
        }
    }
}

void AptIntf::emitRequireRestart(PkgList &output)
{
    // Sort so we can remove the duplicated entries
    output.sort();

    // Remove the duplicated entries
    output.removeDuplicates();

    for (const PkgInfo &info : output) {
        g_autofree gchar *package_id = m_cache->buildPackageId(info.ver);
        pk_backend_job_require_restart(m_job, PK_RESTART_ENUM_SYSTEM, package_id);
    }
}

void AptIntf::emitUpdates(PkgList &output, PkBitfield filters)
{
    PkInfoEnum state;
    // Sort so we can remove the duplicated entries
    output.sort();

    // Remove the duplicated entries
    output.removeDuplicates();

    output = filterPackages(output, filters);
    for (const PkgInfo &pkgInfo : output) {
        if (m_cancel) {
            break;
        }

        // the default update info
        state = PK_INFO_ENUM_NORMAL;

        emitPackage(pkgInfo.ver, state);
    }
}

// search packages which provide a codec (specified in "values")
void AptIntf::providesCodec(PkgList &output, gchar **values)
{
    string arch;
    GstMatcher matcher(values);
    if (!matcher.hasMatches()) {
        return;
    }

    for (pkgCache::PkgIterator pkg = m_cache->GetPkgCache()->PkgBegin(); !pkg.end(); ++pkg) {
        if (m_cancel) {
            break;
        }

        // Ignore packages that exist only due to dependencies.
        if (pkg.VersionList().end() && pkg.ProvidesList().end()) {
            continue;
        }

        // Ignore debug packages - these aren't interesting as codec providers,
        // but they do have apt GStreamer-* metadata.
        if (ends_with (pkg.Name(), "-dbg") || ends_with (pkg.Name(), "-dbgsym")) {
            continue;
        }

        // TODO search in updates packages
        // Ignore virtual packages
        pkgCache::VerIterator ver = m_cache->findVer(pkg);
        if (ver.end() == true) {
            ver = m_cache->findCandidateVer(pkg);
        }
        if (ver.end() == true) {
            continue;
        }

        arch = string(ver.Arch());

        pkgCache::VerFileIterator vf = ver.FileList();
        pkgRecords::Parser &rec = m_cache->GetPkgRecords()->Lookup(vf);
        const char *start, *stop;
        rec.GetRec(start, stop);
        string record(start, stop - start);
        if (matcher.matches(record, arch)) {
            output.append(ver);
        }
    }
}

// search packages which provide the libraries specified in "values"
void AptIntf::providesLibrary(PkgList &output, gchar **values)
{
    bool ret = false;
    // Quick-check for library names
    for (uint i = 0; i < g_strv_length(values); i++) {
        if (g_str_has_prefix(values[i], "lib")) {
            ret = true;
            break;
        }
    }

    if (!ret) {
        return;
    }

    const char *libreg_str = "^\\(lib.*\\)\\.so\\.[0-9]*";
    g_debug("RegStr: %s", libreg_str);
    regex_t libreg;
    if(regcomp(&libreg, libreg_str, 0) != 0) {
        g_debug("Error compiling regular expression to match libraries.");
        return;
    }

    gchar *value;
    for (uint i = 0; i < g_strv_length(values); i++) {
        value = values[i];
        regmatch_t matches[2];
        if (regexec(&libreg, value, 2, matches, 0) != REG_NOMATCH) {
            string libPkgName = string(value, matches[1].rm_so, matches[1].rm_eo - matches[1].rm_so);

            string strvalue = string(value);
            ssize_t pos = strvalue.find (".so.");
            if ((pos > 0) && ((size_t) pos != string::npos)) {
                // If last char is a number, add a "-" (to be policy-compliant)
                if (g_ascii_isdigit (libPkgName.at (libPkgName.length () - 1))) {
                    libPkgName.append ("-");
                }

                libPkgName.append (strvalue.substr (pos + 4));
            }

            g_debug ("pkg-name: %s", libPkgName.c_str ());

            for (pkgCache::PkgIterator pkg = m_cache->GetPkgCache()->PkgBegin(); !pkg.end(); ++pkg) {
                // Ignore packages that exist only due to dependencies.
                if (pkg.VersionList().end() && pkg.ProvidesList().end()) {
                    continue;
                }

                // TODO: Ignore virtual packages
                pkgCache::VerIterator ver = m_cache->findVer(pkg);
                if (ver.end()) {
                    ver = m_cache->findCandidateVer(pkg);
                    if (ver.end()) {
                        continue;
                    }
                }

                // Make everything lower-case
                std::transform(libPkgName.begin(), libPkgName.end(), libPkgName.begin(), ::tolower);

                if (g_strcmp0 (pkg.Name (), libPkgName.c_str ()) == 0) {
                    output.append(ver);
                }
            }
        } else {
            g_debug("libmatcher: Did not match: %s", value);
        }
    }
}

// Mostly copied from pkgAcqArchive.
bool AptIntf::getArchive(pkgAcquire *Owner,
                         const pkgCache::VerIterator &Version,
                         std::string directory,
                         std::string &StoreFilename)
{
    pkgCache::VerFileIterator Vf=Version.FileList();

    if (Version.Arch() == 0) {
        return _error->Error("I wasn't able to locate a file for the %s package. "
                             "This might mean you need to manually fix this package. (due to missing arch)",
                             Version.ParentPkg().Name());
    }

    /* We need to find a filename to determine the extension. We make the
        assumption here that all the available sources for this version share
        the same extension.. */
    // Skip not source sources, they do not have file fields.
    for (; Vf.end() == false; Vf++) {
        if ((Vf.File()->Flags & pkgCache::Flag::NotSource) != 0) {
            continue;
        }
        break;
    }

    // Does not really matter here.. we are going to fail out below
    if (Vf.end() != true) {
        // If this fails to get a file name we will bomb out below.
        pkgRecords::Parser &Parse = m_cache->GetPkgRecords()->Lookup(Vf);
        if (_error->PendingError() == true) {
            return false;
        }

        // Generate the final file name as: package_version_arch.foo
        StoreFilename = QuoteString(Version.ParentPkg().Name(),"_:") + '_' +
                QuoteString(Version.VerStr(),"_:") + '_' +
                QuoteString(Version.Arch(),"_:.") +
                "." + flExtension(Parse.FileName());
    }

    for (; Vf.end() == false; Vf++) {
        // Ignore not source sources
        if ((Vf.File()->Flags & pkgCache::Flag::NotSource) != 0) {
            continue;
        }

        // Try to cross match against the source list
        pkgIndexFile *Index;
        pkgSourceList List;
        if (List.ReadMainList() == false) {
            continue;
        }

        if (List.FindIndex(Vf.File(),Index) == false) {
            continue;
        }

        // Grab the text package record
        pkgRecords::Parser &Parse = m_cache->GetPkgRecords()->Lookup(Vf);
        if (_error->PendingError() == true) {
            return false;
        }

        const string PkgFile = Parse.FileName();
        const std::string hash_md5 = Parse.MD5Hash();
        if (PkgFile.empty() == true) {
            return _error->Error("The package index files are corrupted. No Filename: "
                                 "field for package %s.",
                                 Version.ParentPkg().Name());
        }

        string DestFile = directory + "/" + flNotDir(StoreFilename);

        // Create the item
        new pkgAcqFile(Owner,
                       Index->ArchiveURI(PkgFile),
                       hash_md5,
                       Version->Size,
                       Index->ArchiveInfo(Version),
                       Version.ParentPkg().Name());

        Vf++;
        return true;
    }
    return false;
}

AptCacheFile* AptIntf::aptCacheFile() const
{
    return m_cache.get();
}

// used to emit packages it collects all the needed info
void AptIntf::emitPackageDetail(const pkgCache::VerIterator &ver)
{
    if (ver.end() == true) {
        return;
    }

    const pkgCache::PkgIterator &pkg = ver.ParentPkg();
    std::string section = ver.Section() == NULL ? "" : ver.Section();

    //pkgCache::VerFileIterator vf = ver.FileList();
    //pkgRecords::Parser &rec = m_cache->GetPkgRecords()->Lookup(vf);

    long size;
    if (pkg->CurrentState == pkgCache::State::Installed && pkg.CurrentVer() == ver) {
        // if the package is installed emit the installed size
        size = ver->InstalledSize;
    } else {
        size = ver->Size;
    }

    g_autofree gchar *package_id = m_cache->buildPackageId(ver);
    pk_backend_job_details(m_job,
                           package_id,
                           m_cache->getShortDescription(ver).c_str(),
                           "unknown",
                           get_enum_group(section),
                           m_cache->getLongDescriptionParsed(ver).c_str(),
                           "", //rec.Homepage().c_str(),
                           size);
}

void AptIntf::emitDetails(PkgList &pkgs)
{
    // Sort so we can remove the duplicated entries
    pkgs.sort();

    // Remove the duplicated entries
    pkgs.removeDuplicates();

    for (const PkgInfo &pkgInfo : pkgs) {
        if (m_cancel)
            break;

        emitPackageDetail(pkgInfo.ver);
    }
}

// used to emit packages it collects all the needed info
void AptIntf::emitUpdateDetail(const pkgCache::VerIterator &candver)
{
    // Verify if our update version is valid
    if (candver.end()) {
        // No candidate version was provided
        return;
    }

    const pkgCache::PkgIterator &pkg = candver.ParentPkg();

    // Get the version of the current package
    const pkgCache::VerIterator &currver = m_cache->findVer(pkg);

    // Build a package_id from the current version
    gchar *current_package_id = m_cache->buildPackageId(currver);

    pkgCache::VerFileIterator vf = candver.FileList();
    string origin = vf.File().Origin() == NULL ? "" : vf.File().Origin();
    pkgRecords::Parser &rec = m_cache->GetPkgRecords()->Lookup(candver.FileList());

    string changelog;
    string update_text;
    string updated;
    string issued;
    string srcpkg;
    if (rec.SourcePkg().empty()) {
        srcpkg = pkg.Name();
    } else {
        srcpkg = rec.SourcePkg();
    }

    PkBackend *backend = PK_BACKEND(pk_backend_job_get_backend(m_job));
    if (pk_backend_is_online(backend)) {
        // Create the download object
        AcqPackageKitStatus Stat(this, m_job);

        // get a fetcher
        pkgAcquire fetcher(&Stat);

        // fetch the changelog
        pk_backend_job_set_status(m_job, PK_STATUS_ENUM_DOWNLOAD_CHANGELOG);
        changelog = fetchChangelogData(*m_cache,
                                       fetcher,
                                       candver,
                                       currver,
                                       &update_text,
                                       &updated,
                                       &issued);
    }

    // Check if the update was updates since it was issued
    if (issued.compare(updated) == 0) {
        updated = "";
    }

    // Build a package_id from the update version
    string archive = vf.File().Archive() == NULL ? "" : vf.File().Archive();
    g_autofree gchar *package_id = m_cache->buildPackageId(candver);

    PkUpdateStateEnum updateState = PK_UPDATE_STATE_ENUM_UNKNOWN;
    if (archive.compare("stable") == 0) {
        updateState = PK_UPDATE_STATE_ENUM_STABLE;
    } else if (archive.compare("testing") == 0) {
        updateState = PK_UPDATE_STATE_ENUM_TESTING;
    } else if (archive.compare("unstable")  == 0 ||
               archive.compare("experimental") == 0) {
        updateState = PK_UPDATE_STATE_ENUM_UNSTABLE;
    }

    PkRestartEnum restart = PK_RESTART_ENUM_NONE;
    if (utilRestartRequired(pkg.Name())) {
        restart = PK_RESTART_ENUM_SYSTEM;
    }

    g_auto(GStrv) updates = (gchar **) g_malloc(2 * sizeof(gchar *));
    updates[0] = current_package_id;
    updates[1] = NULL;

    g_autoptr(GPtrArray) bugzilla_urls = getBugzillaUrls(changelog);
    g_autoptr(GPtrArray) cve_urls = getCVEUrls(changelog);
    g_autoptr(GPtrArray) obsoletes = g_ptr_array_new();

    for (auto deps = candver.DependsList(); not deps.end(); ++deps)
    {
        if (deps->Type == pkgCache::Dep::Obsoletes)
        {
            g_ptr_array_add(obsoletes, (void*) deps.TargetPkg().Name());
        }
    }

    // NULL terminate
    g_ptr_array_add(obsoletes, NULL);

    pk_backend_job_update_detail(m_job,
                                 package_id,
                                 updates,//const gchar *updates
                                 (gchar **) obsoletes->pdata,//const gchar *obsoletes
                                 NULL,//const gchar *vendor_url
                                 (gchar **) bugzilla_urls->pdata,// gchar **bugzilla_urls
                                 (gchar **) cve_urls->pdata,// gchar **cve_urls
                                 restart,//PkRestartEnum restart
                                 update_text.c_str(),//const gchar *update_text
                                 changelog.c_str(),//const gchar *changelog
                                 updateState,//PkUpdateStateEnum state
                                 issued.c_str(), //const gchar *issued_text
                                 updated.c_str() //const gchar *updated_text
                                 );
}

void AptIntf::emitUpdateDetails(const PkgList &pkgs)
{
    for (const PkgInfo &pi : pkgs) {
        if (m_cancel)
            break;
        emitUpdateDetail(pi.ver);
    }
}

void AptIntf::getDepends(PkgList &output,
                         const pkgCache::VerIterator &ver,
                         bool recursive)
{
    pkgCache::DepIterator dep = ver.DependsList();
    while (!dep.end()) {
        if (m_cancel) {
            break;
        }

        const pkgCache::VerIterator &ver = m_cache->findVer(dep.TargetPkg());
        // Ignore packages that exist only due to dependencies.
        if (ver.end()) {
            dep++;
            continue;
        } else if (dep->Type == pkgCache::Dep::Depends) {
            if (recursive) {
                if (!output.contains(dep.TargetPkg())) {
                    output.append(ver);
                    getDepends(output, ver, recursive);
                }
            } else {
                output.append(ver);
            }
        }
        dep++;
    }
}

void AptIntf::getRequires(PkgList &output,
                          const pkgCache::VerIterator &ver,
                          bool recursive)
{
    for (pkgCache::PkgIterator parentPkg = m_cache->GetPkgCache()->PkgBegin(); !parentPkg.end(); ++parentPkg) {
        if (m_cancel) {
            break;
        }

        // Ignore packages that exist only due to dependencies.
        if (parentPkg.VersionList().end() && parentPkg.ProvidesList().end()) {
            continue;
        }

        // Don't insert virtual packages instead add what it provides
        const pkgCache::VerIterator &parentVer = m_cache->findVer(parentPkg);
        if (parentVer.end() == false) {
            PkgList deps;
            getDepends(deps, parentVer, false);
            for (const PkgInfo &depInfo : deps) {
                if (depInfo.ver == ver) {
                    if (recursive) {
                        if (!output.contains(parentPkg)) {
                            output.append(parentVer);
                            getRequires(output, parentVer, recursive);
                        }
                    } else {
                        output.append(parentVer);
                    }
                    break;
                }
            }
        }
    }
}

PkgList AptIntf::getPackages()
{
    pk_backend_job_set_status(m_job, PK_STATUS_ENUM_QUERY);

    PkgList output;
    output.reserve(m_cache->GetPkgCache()->HeaderP->PackageCount);
    for (pkgCache::PkgIterator pkg = m_cache->GetPkgCache()->PkgBegin(); !pkg.end(); ++pkg) {
        if (m_cancel) {
            break;
        }

        // Ignore packages that exist only due to dependencies.
        if(pkg.VersionList().end() && pkg.ProvidesList().end()) {
            continue;
        }

        // Don't insert virtual packages as they don't have all kinds of info
        const pkgCache::VerIterator &ver = m_cache->findVer(pkg);
        if (ver.end() == false)
            output.append(ver);
    }
    return output;
}

PkgList AptIntf::getPackagesFromRepo(SourcesList::SourceRecord *&rec)
{
    pk_backend_job_set_status(m_job, PK_STATUS_ENUM_QUERY);

    PkgList output;
    output.reserve(m_cache->GetPkgCache()->HeaderP->PackageCount);
    for (pkgCache::PkgIterator pkg = m_cache->GetPkgCache()->PkgBegin(); !pkg.end(); ++pkg) {
        if (m_cancel) {
            break;
        }

        // Ignore packages that exist only due to dependencies.
        if(pkg.VersionList().end() && pkg.ProvidesList().end()) {
            continue;
        }

        // Don't insert virtual packages as they don't have all kinds of info
        const pkgCache::VerIterator &ver = m_cache->findVer(pkg);
        if (ver.end()) {
            continue;
        }

        // only installed packages matters
        if (!(pkg->CurrentState == pkgCache::State::Installed && pkg.CurrentVer() == ver)) {
            continue;
        }

        // Distro name
        pkgCache::VerFileIterator vf = ver.FileList();
        if (vf.File().Archive() == NULL || rec->Dist.compare(vf.File().Archive()) != 0){
            continue;
        }

        // Section part
        if (vf.File().Component() == NULL || !rec->hasSection(vf.File().Component())) {
            continue;
        }

        // Check if the site the package comes from is include in the Repo uri
        if (vf.File().Site() == NULL || rec->URI.find(vf.File().Site()) == std::string::npos) {
            continue;
        }

        output.append(ver);
    }
    return output;
}

PkgList AptIntf::getPackagesFromGroup(gchar **values)
{
    pk_backend_job_set_status(m_job, PK_STATUS_ENUM_QUERY);

    PkgList output;
    vector<PkGroupEnum> groups;

    uint len = g_strv_length(values);
    for (uint i = 0; i < len; i++) {
        if (values[i] == NULL) {
            pk_backend_job_error_code(m_job,
                                      PK_ERROR_ENUM_GROUP_NOT_FOUND,
                                      "An empty group was received");
            return output;
        } else {
            groups.push_back(pk_group_enum_from_string(values[i]));
        }
    }

    pk_backend_job_set_allow_cancel(m_job, true);

    for (pkgCache::PkgIterator pkg = m_cache->GetPkgCache()->PkgBegin(); !pkg.end(); ++pkg) {
        if (m_cancel) {
            break;
        }
        // Ignore packages that exist only due to dependencies.
        if (pkg.VersionList().end() && pkg.ProvidesList().end()) {
            continue;
        }

        // Ignore virtual packages
        const pkgCache::VerIterator &ver = m_cache->findVer(pkg);
        if (ver.end() == false) {
            string section = pkg.VersionList().Section() == NULL ? "" : pkg.VersionList().Section();

            // Don't insert virtual packages instead add what it provides
            for (PkGroupEnum group : groups) {
                if (group == get_enum_group(section)) {
                    output.append(ver);
                    break;
                }
            }
        }
    }
    return output;
}

bool AptIntf::matchesQueries(const vector<string> &queries, string s) {
    for (string query : queries) {
        // Case insensitive "string.contains"
        auto it = std::search(
            s.begin(), s.end(),
            query.begin(), query.end(),
            [](unsigned char ch1, unsigned char ch2) {
                return std::tolower(ch1) == std::tolower(ch2);
            }
        );

        if (it != s.end()) {
            return true;
        }
    }
    return false;
}

PkgList AptIntf::searchPackageName(const vector<string> &queries)
{
    PkgList output;

    for (pkgCache::PkgIterator pkg = m_cache->GetPkgCache()->PkgBegin(); !pkg.end(); ++pkg) {
        if (m_cancel) {
            break;
        }
        // Ignore packages that exist only due to dependencies.
        if (pkg.VersionList().end() && pkg.ProvidesList().end()) {
            continue;
        }

        if (matchesQueries(queries, pkg.Name())) {
            // Don't insert virtual packages instead add what it provides
            const pkgCache::VerIterator &ver = m_cache->findVer(pkg);
            if (ver.end() == false) {
                output.append(ver);
            } else {
                // iterate over the provides list
                for (pkgCache::PrvIterator Prv = pkg.ProvidesList(); Prv.end() == false; ++Prv) {
                    const pkgCache::VerIterator &ownerVer = m_cache->findVer(Prv.OwnerPkg());

                    // check to see if the provided package isn't virtual too
                    if (ownerVer.end() == false) {
                        // we add the package now because we will need to
                        // remove duplicates later anyway
                        output.append(ownerVer);
                    }
                }
            }
        }
    }
    return output;
}

PkgList AptIntf::searchPackageDetails(const vector<string> &queries)
{
    PkgList output;

    for (pkgCache::PkgIterator pkg = m_cache->GetPkgCache()->PkgBegin(); !pkg.end(); ++pkg) {
        if (m_cancel) {
            break;
        }
        // Ignore packages that exist only due to dependencies.
        if (pkg.VersionList().end() && pkg.ProvidesList().end()) {
            continue;
        }

        const pkgCache::VerIterator &ver = m_cache->findVer(pkg);
        if (ver.end() == false) {
            if (matchesQueries(queries, pkg.Name()) ||
                    matchesQueries(queries, (*m_cache).getLongDescription(ver))) {
                // The package matched
                output.append(ver);
            }
        } else if (matchesQueries(queries, pkg.Name())) {
            // The package is virtual and MATCHED the name
            // Don't insert virtual packages instead add what it provides

            // iterate over the provides list
            for (pkgCache::PrvIterator Prv = pkg.ProvidesList(); Prv.end() == false; ++Prv) {
                const pkgCache::VerIterator &ownerVer = m_cache->findVer(Prv.OwnerPkg());

                // check to see if the provided package isn't virtual too
                if (ownerVer.end() == false) {
                    // we add the package now because we will need to
                    // remove duplicates later anyway
                    output.append(ownerVer);
                }
            }
        }
    }
    return output;
}

PkgList AptIntf::getUpdates(PkgList &blocked, PkgList &downgrades, PkgList &installs, PkgList &removals, PkgList &obsoleted)
{
    PkgList updates;

    if (m_cache->DistUpgrade() == false) {
        m_cache->ShowBroken(false);
        g_debug("Internal error, DistUpgrade broke stuff");
        cout << "Internal error, DistUpgrade broke stuff" << endl;
        return updates;
    }

    for (pkgCache::PkgIterator pkg = (*m_cache)->PkgBegin(); !pkg.end(); ++pkg) {
        const auto &state = (*m_cache)[pkg];
        if (pkg->SelectedState == pkgCache::State::Hold) {
            // We pretend held packages are not upgradable at all since we can't represent
            // the concept of holds in PackageKit.
            // https://github.com/PackageKit/PackageKit/issues/120
            continue;
        } else if (state.Upgrade() == true && state.NewInstall() == false) {
            const pkgCache::VerIterator &ver = m_cache->findCandidateVer(pkg);
            if (!ver.end()) {
                updates.append(ver);
            }
        } else if (state.Downgrade() == true) {
            const pkgCache::VerIterator &ver = m_cache->findCandidateVer(pkg);
            if (!ver.end()) {
                downgrades.append(ver);
            }
        } else if (state.Upgradable() == true &&
                   pkg->CurrentVer != 0 &&
                   state.Delete() == false) {
            const pkgCache::VerIterator &ver = m_cache->findCandidateVer(pkg);
            if (!ver.end()) {
                blocked.append(ver);
            }
        } else if (state.NewInstall()) {
            const pkgCache::VerIterator &ver = m_cache->findCandidateVer(pkg);
            if (!ver.end()) {
                installs.append(ver);
            }
        } else if (state.Delete()) {
            const pkgCache::VerIterator &ver = m_cache->findCandidateVer(pkg);
            if (!ver.end()) {
                bool is_obsoleted = false;

                /* Following code fragment should be similar to pkgDistUpgrade's one */
                for (pkgCache::DepIterator D = pkg.RevDependsList(); not D.end(); ++D)
                {
                    if ((D->Type == pkgCache::Dep::Obsoletes)
                        && ((*m_cache)[D.ParentPkg()].CandidateVer != nullptr)
                        && (*m_cache)[D.ParentPkg()].CandidateVerIter(*m_cache).Downloadable()
                        && ((pkgCache::Version*)D.ParentVer() == (*m_cache)[D.ParentPkg()].CandidateVer)
                        && (*m_cache)->VS().CheckDep(pkg.CurrentVer().VerStr(), D)
                        && ((*m_cache)->GetPkgPriority(D.ParentPkg()) >= (*m_cache)->GetPkgPriority(pkg)))
                    {
                        is_obsoleted = true;
                        break;
                    }
                }

                if (is_obsoleted ) {
                    /* Obsoleted packages */
                    obsoleted.append(ver);
                } else {
                    /* Removed packages */
                    removals.append(ver);
                }
            }
        }
    }

    return updates;
}

// used to return files it reads, using the info from the files in /var/lib/dpkg/info/
void AptIntf::providesMimeType(PkgList &output, gchar **values)
{
    g_autoptr(AsPool) pool = NULL;
    g_autoptr(GError) error = NULL;
    std::vector<string> pkg_names;

    pool = as_pool_new ();

    /* don't monitor cache locations or load Flatpak data */
    as_pool_remove_flags (pool, AS_POOL_FLAG_MONITOR);
    as_pool_remove_flags (pool, AS_POOL_FLAG_LOAD_FLATPAK);

    /* try to load the metadata pool */
    if (!as_pool_load (pool, NULL, &error)) {
        pk_backend_job_error_code(m_job,
                                  PK_ERROR_ENUM_INTERNAL_ERROR,
                                  "Failed to load AppStream metadata: %s", error->message);
        return;
    }

    /* search for mimetypes for all values */
    for (guint i = 0; values[i] != NULL; i++) {
#if AS_CHECK_VERSION(1,0,0)
        g_autoptr(AsComponentBox) result = NULL;
#else
        g_autoptr(GPtrArray) result = NULL;
#endif
        if (m_cancel)
            break;

#if AS_CHECK_VERSION(1,0,0)
        result = as_pool_get_components_by_provided_item (pool, AS_PROVIDED_KIND_MEDIATYPE, values[i]);
        for (guint j = 0; j < as_component_box_len (result); j++) {
            const gchar *pkgname;
            AsComponent *cpt = as_component_box_index (result, j);
#else
        result = as_pool_get_components_by_provided_item (pool, AS_PROVIDED_KIND_MEDIATYPE, values[i]);
        for (guint j = 0; j < result->len; j++) {
            const gchar *pkgname;
            AsComponent *cpt = AS_COMPONENT (g_ptr_array_index (result, j));
#endif
            /* sanity check */
            pkgname = as_component_get_pkgname (cpt);
            if (pkgname == NULL) {
                g_warning ("Component %s has no package name (it was ignored in the search).", as_component_get_data_id (cpt));
                continue;
            }

            pkg_names.push_back (pkgname);
        }
    }

    /* resolve the package names */
    for (const std::string &package : pkg_names) {
        if (m_cancel)
            break;

        const pkgCache::PkgIterator &pkg = (*m_cache)->FindPkg(package);
        if (pkg.end() == true)
            continue;
        const pkgCache::VerIterator &ver = m_cache->findVer(pkg);
        if (ver.end() == true)
            continue;

        output.append(ver);
    }
}

/**
 * checkChangedPackages - Check whas is goind to happen to the packages
 */
PkgList AptIntf::checkChangedPackages(bool emitChanged)
{
    PkgList ret;
    PkgList installing;
    PkgList removing;
    PkgList updating;
    PkgList downgrading;
    PkgList obsoleting;

    for (pkgCache::PkgIterator pkg = (*m_cache)->PkgBegin(); ! pkg.end(); ++pkg) {
        if ((*m_cache)[pkg].NewInstall() == true) {
            // installing;
            const pkgCache::VerIterator &ver = m_cache->findCandidateVer(pkg);
            if (!ver.end()) {
                ret.append(ver);
                installing.append(ver);

                // append to the restart required list
                if (utilRestartRequired(pkg.Name())) {
                    m_restartPackages.append(ver);
                }
            }
        } else if ((*m_cache)[pkg].Delete() == true) {
            // removing
            const pkgCache::VerIterator &ver = m_cache->findVer(pkg);
            if (!ver.end()) {
                ret.append(ver);

                bool is_obsoleted = false;

                /* Following code fragment should be similar to pkgDistUpgrade's one */
                for (pkgCache::DepIterator D = pkg.RevDependsList(); not D.end(); ++D)
                {
                    if ((D->Type == pkgCache::Dep::Obsoletes)
                            && ((*m_cache)[D.ParentPkg()].CandidateVer != nullptr)
                            && (*m_cache)[D.ParentPkg()].CandidateVerIter(*m_cache).Downloadable()
                            && ((pkgCache::Version*)D.ParentVer() == (*m_cache)[D.ParentPkg()].CandidateVer)
                            && (*m_cache)->VS().CheckDep(pkg.CurrentVer().VerStr(), D)
                            && ((*m_cache)->GetPkgPriority(D.ParentPkg()) >= (*m_cache)->GetPkgPriority(pkg)))
                    {
                        is_obsoleted = true;
                        break;
                    }
                }

                if (!is_obsoleted) {
                    removing.append(ver);
                } else {
                    obsoleting.append(ver);
                }

                // append to the restart required list
                if (utilRestartRequired(pkg.Name())) {
                    m_restartPackages.append(ver);
                }
            }
        } else if ((*m_cache)[pkg].Upgrade() == true) {
            // updating
            const pkgCache::VerIterator &ver = m_cache->findCandidateVer(pkg);
            if (!ver.end()) {
                ret.append(ver);
                updating.append(ver);

                // append to the restart required list
                if (utilRestartRequired(pkg.Name()))
                    m_restartPackages.append(ver);
            }
        } else if ((*m_cache)[pkg].Downgrade() == true) {
            // downgrading
            const pkgCache::VerIterator &ver = m_cache->findCandidateVer(pkg);
            if (!ver.end()) {
                ret.append(ver);
                downgrading.append(ver);

                // append to the restart required list
                if (utilRestartRequired(pkg.Name())) {
                    m_restartPackages.append(ver);
                }
            }
        }
    }

    if (emitChanged) {
        // emit packages that have changes
        emitPackages(obsoleting,  PK_FILTER_ENUM_NONE, PK_INFO_ENUM_OBSOLETING);
        emitPackages(removing,    PK_FILTER_ENUM_NONE, PK_INFO_ENUM_REMOVING);
        emitPackages(downgrading, PK_FILTER_ENUM_NONE, PK_INFO_ENUM_DOWNGRADING);
        emitPackages(installing,  PK_FILTER_ENUM_NONE, PK_INFO_ENUM_INSTALLING);
        emitPackages(updating,    PK_FILTER_ENUM_NONE, PK_INFO_ENUM_UPDATING);
    }

    return ret;
}

pkgCache::VerIterator AptIntf::findTransactionPackage(const std::string &name)
{
    for (const PkgInfo &pkInfo : m_pkgs) {
        if (pkInfo.ver.ParentPkg().Name() == name) {
            return pkInfo.ver;
        }
    }

    const pkgCache::PkgIterator &pkg = (*m_cache)->FindPkg(name);
    // Ignore packages that could not be found or that exist only due to dependencies.
    if (pkg.end() == true ||
            (pkg.VersionList().end() && pkg.ProvidesList().end())) {
        return pkgCache::VerIterator();
    }

    const pkgCache::VerIterator &ver = m_cache->findVer(pkg);
    // check to see if the provided package isn't virtual too
    if (ver.end() == false) {
        return ver;
    }

    const pkgCache::VerIterator &candidateVer = m_cache->findCandidateVer(pkg);

    // Return the last try anyway
    return candidateVer;
}

PkgList AptIntf::resolvePackageIds(gchar **package_ids, PkBitfield filters)
{
    PkgList ret;

    pk_backend_job_set_status (m_job, PK_STATUS_ENUM_QUERY);

    // Don't fail if package list is empty
    if (package_ids == NULL)
        return ret;

    for (uint i = 0; i < g_strv_length(package_ids); ++i) {
        if (m_cancel)
            break;

        const gchar *pkgid = package_ids[i];

        // Check if it's a valid package id
        if (pk_package_id_check(pkgid) == false) {
            string name(pkgid);
                const pkgCache::PkgIterator &pkg = (*m_cache)->FindPkg(name);
                // Ignore packages that could not be found or that exist only due to dependencies.
                if (pkg.end() == true || (pkg.VersionList().end() && pkg.ProvidesList().end())) {
                    continue;
                }

                const pkgCache::VerIterator &ver = m_cache->findVer(pkg);
                // check to see if the provided package isn't virtual too
                if (ver.end() == false)
                    ret.append(ver);

                const pkgCache::VerIterator &candidateVer = m_cache->findCandidateVer(pkg);
                // check to see if the provided package isn't virtual too
                if (candidateVer.end() == false)
                    ret.append(candidateVer);
        } else {
            const PkgInfo &pkgi = m_cache->resolvePkgID(pkgid);
            // check to see if we found the package
            if (!pkgi.ver.end())
                ret.append(pkgi);
        }
    }

    return filterPackages(ret, filters);
}

void AptIntf::refreshCache()
{
    pk_backend_job_set_status(m_job, PK_STATUS_ENUM_REFRESH_CACHE);

    // Rebuild the cache.

    // First, destroy m_cache which holds the old copy of the pkgCache and
    // dependent objects. Note that BuildCaches() alone won't overwrite the held
    // pkgCache if it's already computed (per Debian's current APT API).
    m_cache.reset();

    pkgCacheFile::RemoveCaches();

    m_cache.reset(new AptCacheFile(m_job, true /* withLock */, &m_progress));

    if (m_cache->BuildSourceList() == false) {
        return;
    }

    // Create the progress
    AcqPackageKitStatus Stat(this, m_job);

    // do the work
    ListUpdate(Stat, *m_cache->GetSourceList(), *m_cache);

    // Setting WithLock implies not AllowMem in APT. (Building in memory only
    // would be a waste of time for refreshCache(): nothing saved to disk.)
    // So does apt-get update, too.

    // Ultimately, force the cache to be computed.
    if (m_cache->BuildCaches() == false) {
        return;
    }
}

void AptIntf::markAutoInstalled(const PkgList &pkgs)
{
    for (const PkgInfo &pkInfo : pkgs) {
        if (m_cancel)
            break;

        // Mark package as auto-installed
        (*m_cache)->MarkAuto(pkInfo.ver.ParentPkg(), pkgDepCache::AutoMarkFlag::Auto);
    }
}

bool AptIntf::runTransaction(const PkgList &install, const PkgList &remove, const PkgList &update,
                             bool fixBroken, PkBitfield flags, bool autoremove)
{
    pk_backend_job_set_status (m_job, PK_STATUS_ENUM_RUNNING);

    // Enter the special broken fixing mode if the user specified arguments
    // THIS mode will run if fixBroken is false and the cache has broken packages
    bool attemptFixBroken = false;
    if ((*m_cache)->BrokenCount() != 0) {
        attemptFixBroken = true;
    }

    pkgProblemResolver Fix(*m_cache);

    // TODO: could use std::bind an have a generic operation array iff toRemove had the same
    //       signature

    struct Operation {
        const PkgList &list;
        const bool preserveAuto;
    };

    // Calculate existing garbage before the transaction
    std::set<std::string> initial_garbage;
    if (autoremove) {
        if (!pkgAutoremoveGetKeptAndUnneededPackages(*m_cache, nullptr, &initial_garbage)) {
            return false;
        }
    }

    // new scope for the ActionGroup
    {
        m_progress.OverallProgress(0, install.size() + remove.size() + update.size(), 1, "updating");
        unsigned long long processed_packages = 0;
        for (auto op : { Operation { install, false }, Operation { update, true } }) {
            // We first need to mark all manual selections with AutoInst=false, so they influence which packages
            // are chosen when resolving dependencies.
            // Consider A depends X|Y, with installation of A,Y requested.
            // With just one run and AutoInst=true, A would be marked for install, it would auto-install X;
            // then Y is marked for install, and we end up with both X and Y marked for install.
            // With two runs (one without AutoInst and one with AutoInst), we first mark A and Y for install.
            // In the 2nd run, when resolving X|Y APT notices that X is already marked for install, and does not install Y.
            for (auto autoInst : { false, true }) {
                for (const PkgInfo &pkInfo : op.list) {
                    if (m_cancel) {
                        break;
                    }
                    if (!m_cache->tryToInstall(Fix,
                                               pkInfo,
                                               autoInst,
                                               op.preserveAuto,
                                               attemptFixBroken)) {
                        return false;
                    }
                    m_progress.Progress(++processed_packages);
                }
            }
        }

        for (const PkgInfo &pkInfo : remove) {
            if (m_cancel)
                break;

            m_cache->tryToRemove(Fix, pkInfo);
            m_progress.Progress(++processed_packages);
        }

        // Call the scored problem resolver
        if (Fix.Resolve(true) == false) {
            _error->Discard();
        }

        // Now we check the state of the packages,
        if ((*m_cache)->BrokenCount() != 0) {
            // if the problem resolver could not fix all broken things
            // suggest to run RepairSystem by saying that the last transaction
            // did not finish well
            m_cache->ShowBroken(false, PK_ERROR_ENUM_DEP_RESOLUTION_FAILED);
            return false;
        }
    }

    // Remove new garbage that is created
    if (autoremove) {
        std::set<std::string> new_garbage;
        if (!pkgAutoremoveGetKeptAndUnneededPackages(*m_cache, nullptr, &new_garbage)) {
            return false;
        }

        for (pkgCache::PkgIterator pkg = (*m_cache)->PkgBegin(); ! pkg.end(); ++pkg) {
            const pkgCache::VerIterator &ver = pkg.CurrentVer();
            if (!ver.end() && (initial_garbage.find(pkg.Name()) == initial_garbage.end()) && (new_garbage.find(pkg.Name()) != new_garbage.end()))
                m_cache->tryToRemove (Fix, PkgInfo(ver));
        }
    }

    // Prepare for the restart thing
    struct stat restartStatStart;
    if (g_file_test(REBOOT_REQUIRED, G_FILE_TEST_EXISTS)) {
        g_stat(REBOOT_REQUIRED, &restartStatStart);
    }

    // If we are simulating the install packages
    // will just calculate the trusted packages
    const auto ret = installPackages(flags);

    if (g_file_test(REBOOT_REQUIRED, G_FILE_TEST_EXISTS)) {
        struct stat restartStat;
        g_stat(REBOOT_REQUIRED, &restartStat);

        if (restartStat.st_mtime > restartStatStart.st_mtime) {
            // Emit the packages that caused the restart
            if (!m_restartPackages.empty()) {
                emitRequireRestart(m_restartPackages);
            } else if (!m_pkgs.empty()) {
                // Assume all of them
                emitRequireRestart(m_pkgs);
            } else {
                // Emit a foo require restart
                pk_backend_job_require_restart(m_job, PK_RESTART_ENUM_SYSTEM, "aptcc;;;");
            }
        }
    }

    return ret;
}

void AptIntf::showProgress(const char *nevra,
                           const aptCallbackType what,
                           const uint64_t amount,
                           const uint64_t total,
                           void *data)
{
    OpProgress *progress = (OpProgress *) data;
    switch (what) {
    case APTCALLBACK_ELEM_PROGRESS:
        progress->OverallProgress(amount, total, 1, "Installing updates");
        break;
    default:
        break;
    }
}

/**
 * InstallPackages - Download and install the packages
 *
 * This displays the informative messages describing what is going to
 * happen and then calls the download routines
 */
bool AptIntf::installPackages(PkBitfield flags)
{
    bool simulate = pk_bitfield_contain(flags, PK_TRANSACTION_FLAG_ENUM_SIMULATE);
    PkBackend *backend = PK_BACKEND(pk_backend_job_get_backend(m_job));

    //cout << "installPackages() called" << endl;

    // check for essential packages!!!
    if (m_cache->isRemovingEssentialPackages()) {
        return false;
    }

#ifdef WITH_LUA
    _lua->SetDepCache(*m_cache);
    _lua->RunScripts("Scripts::PackageKit::RunTransaction::Pre");
    _lua->ResetCaches();
#endif

    // Sanity check
    if ((*m_cache)->BrokenCount() != 0) {
        // TODO
        m_cache->ShowBroken(false);
        _error->Error("Internal error, InstallPackages was called with broken packages!");
        return false;
    }

    if ((*m_cache)->DelCount() == 0 && (*m_cache)->InstCount() == 0 &&
            (*m_cache)->BadCount() == 0) {
        return true;
    }

    // Create the download object
    AcqPackageKitStatus Stat(this, m_job);

    // get a fetcher
    pkgAcquire fetcher(&Stat);
    FileFd Lock;
    if (!simulate) {
        // Only lock the archive directory if we will download
        // Lock the archive directory
        if (_config->FindB("Debug::NoLocking",false) == false)
        {
            Lock.Fd(GetLock(_config->FindDir("Dir::Cache::Archives") + "lock"));
            if (_error->PendingError() == true)
            {
                return _error->Error("Unable to lock the download directory");
            }
        }
    }

    pkgSourceList List;
    if (List.ReadMainList() == false) {
        return false;
    }

    // Create the package manager and prepare to download
    std::unique_ptr<pkgPackageManager> PM (_system->CreatePM(*m_cache));
    if (!PM->GetArchives(&fetcher, &List, m_cache->GetPkgRecords()) ||
            _error->PendingError() == true) {
        return false;
    }

    // Display statistics
    unsigned long long FetchBytes = fetcher.FetchNeeded();
    unsigned long long FetchPBytes = fetcher.PartialPresent();
    unsigned long long DebBytes = fetcher.TotalNeeded();
    if (DebBytes != (*m_cache)->DebSize()) {
        cout << DebBytes << ',' << (*m_cache)->DebSize() << endl;
        cout << "How odd.. The sizes didn't match, email apt@packages.debian.org";
    }

    // Number of bytes
    if (FetchBytes != 0) {
        // Emit the remainig download size
        pk_backend_job_set_download_size_remaining(m_job, FetchBytes);

        // check network state if we are going to download
        // something or if we are not simulating
        if (!simulate && !pk_backend_is_online(backend)) {
            pk_backend_job_error_code(m_job,
                                      PK_ERROR_ENUM_NO_NETWORK,
                                      "Cannot download packages whilst offline");
            return false;
        }
    }

    /* Check for enough free space */
    struct statvfs Buf;
    string OutputDir = _config->FindDir("Dir::Cache::Archives");
    if (statvfs(OutputDir.c_str(),&Buf) != 0) {
        return _error->Errno("statvfs",
                             "Couldn't determine free space in %s",
                             OutputDir.c_str());
    }
    if (unsigned(Buf.f_bfree) < (FetchBytes - FetchPBytes)/Buf.f_bsize) {
        struct statfs Stat;
        if (statfs(OutputDir.c_str(), &Stat) != 0 ||
                unsigned(Stat.f_type) != RAMFS_MAGIC) {
            pk_backend_job_error_code(m_job,
                                      PK_ERROR_ENUM_NO_SPACE_ON_DEVICE,
                                      "You don't have enough free space in %s",
                                      OutputDir.c_str());
            return false;
        }
    }

    if (_error->PendingError() == true) {
        cout << "PendingError " << endl;
        return false;
    }

    if (simulate) {
        // Print out a list of packages that are going to be installed extra
        checkChangedPackages(true);

        return true;
    } else {
        // Store the packages that are going to change
        // so we can emit them as we process it
        m_pkgs = checkChangedPackages(false);
    }

    // Download and check if we can continue
    if (fetcher.Run() != pkgAcquire::Continue
            && m_cancel == false) {
        // We failed and we did not cancel
        show_errors(m_job, PK_ERROR_ENUM_PACKAGE_DOWNLOAD_FAILED);
        return false;
    }

    if (_error->PendingError() == true) {
        cout << "PendingError download" << endl;
        return false;
    }

    // Download finished, check if we should proceed the install
    if (pk_bitfield_contain(flags, PK_TRANSACTION_FLAG_ENUM_ONLY_DOWNLOAD)) {
        return true;
    }

    // Check if the user canceled
    if (m_cancel) {
        return true;
    }

    // Right now it's not safe to cancel
    pk_backend_job_set_allow_cancel(m_job, false);

    // Download should be finished by now, changing it's status
    pk_backend_job_set_percentage(m_job, PK_BACKEND_PERCENTAGE_INVALID);

    _system->UnLock();

    PM->DoInstall(showProgress, &m_progress);

    return true;
}
