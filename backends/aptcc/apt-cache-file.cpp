/* apt-cache-file.cpp
 *
 * Copyright (c) 2012 Daniel Nicoletti <dantti12@gmail.com>
 * Copyright (c) 2012 Matthias Klumpp <matthias@tenstral.net>
 * Copyright (c) 2016 Harald Sitter <sitter@kde.org>
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
#include "apt-cache-file.h"

#include <sstream>
#include <cstdio>
#include <apt-pkg/algorithms.h>
#include <apt-pkg/progress.h>
#include <apt-pkg/error.h>
#include <apt-pkg/version.h>

#include "apt-utils.h"
#include "apt-messages.h"

AptCacheFile::AptCacheFile(PkBackendJob *job, bool const withLock,
                           OpPackageKitProgress *progress) :
    pkgCacheFile(withLock),
    m_packageRecords(0),
    m_job(job),
    m_progress(progress)
{
}

AptCacheFile::~AptCacheFile()
{
    delete m_packageRecords;

    m_packageRecords = 0;

    // Discard all errors to avoid a future failure when opening
    // the package cache
    _error->Discard();
}

bool AptCacheFile::Open()
{
    return pkgCacheFile::Open(*m_progress);
}

bool AptCacheFile::BuildCaches()
{
    return pkgCacheFile::BuildCaches(*m_progress);
}

pkgCache* AptCacheFile::GetPkgCache()
{
    return pkgCacheFile::GetPkgCache(*m_progress);
}

pkgPolicy* AptCacheFile::GetPolicy()
{
    return pkgCacheFile::GetPolicy(*m_progress);
}

pkgDepCache* AptCacheFile::GetDepCache()
{
    return pkgCacheFile::GetDepCache(*m_progress);
}

bool AptCacheFile::CheckDeps(bool AllowBroken)
{
    if (_error->PendingError() == true) {
        return false;
    }

    // Check that the system is OK
    if (getDCache()->DelCount() != 0 || getDCache()->InstCount() != 0) {
        _error->Error("Internal error, non-zero counts");
        show_errors(m_job, PK_ERROR_ENUM_INTERNAL_ERROR);
        return false;
    }

    // Apply corrections for half-installed packages
    if (pkgApplyStatus(*getDCache()) == false) {
        _error->Error("Unable to apply corrections for half-installed packages");;
        show_errors(m_job, PK_ERROR_ENUM_INTERNAL_ERROR);
        return false;
    }

    // Nothing is broken or we don't want to try fixing it
    if (getDCache()->BrokenCount() == 0 || AllowBroken == true) {
        return true;
    }

    // Attempt to fix broken things
    if (pkgFixBroken(*getDCache()) == false || getDCache()->BrokenCount() != 0) {
        // We failed to fix the cache
        ShowBroken(true, PK_ERROR_ENUM_UNFINISHED_TRANSACTION);

        g_warning("Unable to correct dependencies");
        return false;
    }

    if (pkgMinimizeUpgrade(*getDCache()) == false) {
        g_warning("Unable to minimize the upgrade set");
        show_errors(m_job, PK_ERROR_ENUM_INTERNAL_ERROR);
        return false;
    }

    // Fixing the cache is DONE no errors were found
    return true;
}

bool AptCacheFile::DistUpgrade()
{
    return pkgDistUpgrade(*getDCache());
}

void AptCacheFile::ShowBroken(bool Now, PkErrorEnum error)
{
    std::stringstream out;

    out << "The following packages have unmet dependencies:" << std::endl;
    for (pkgCache::PkgIterator I = (*this)->PkgBegin(); ! I.end(); ++I) {
        if (Now == true) {
            if ((*this)[I].NowBroken() == false) {
                continue;
            }
        } else {
            if ((*this)[I].InstBroken() == false){
                continue;
            }
        }

        // Print out each package and the failed dependencies
        out << "  " <<  I.Name() << ":";
        unsigned Indent = strlen(I.Name()) + 3;
        bool First = true;
        pkgCache::VerIterator Ver;

        if (Now == true) {
            Ver = I.CurrentVer();
        } else {
            Ver = (*this)[I].InstVerIter(*this);
        }

        if (Ver.end() == true) {
            out << std::endl;
            continue;
        }

        for (pkgCache::DepIterator D = Ver.DependsList(); D.end() == false;) {
            // Compute a single dependency element (glob or)
            pkgCache::DepIterator Start;
            pkgCache::DepIterator End;
            D.GlobOr(Start,End); // advances D

            if ((*this)->IsImportantDep(End) == false){
                continue;
            }

            if (Now == true) {
                if (((*this)[End] & pkgDepCache::DepGNow) == pkgDepCache::DepGNow){
                    continue;
                }
            } else {
                if (((*this)[End] & pkgDepCache::DepGInstall) == pkgDepCache::DepGInstall) {
                    continue;
                }
            }

            bool FirstOr = true;
            while (1) {
                if (First == false){
                    for (unsigned J = 0; J != Indent; J++) {
                        out << ' ';
                    }
                }
                First = false;

                if (FirstOr == false) {
                    for (unsigned J = 0; J != strlen(End.DepType()) + 3; J++) {
                        out << ' ';
                    }
                } else {
                    out << ' ' << End.DepType() << ": ";
                }
                FirstOr = false;

                out << Start.TargetPkg().Name();

                // Show a quick summary of the version requirements
                if (Start.TargetVer() != 0) {
                    out << " (" << Start.CompType() << " " << Start.TargetVer() << ")";
                }

                /* Show a summary of the target package if possible. In the case
                of virtual packages we show nothing */
                pkgCache::PkgIterator Targ = Start.TargetPkg();
                if (Targ->ProvidesList == 0) {
                    out << ' ';
                    pkgCache::VerIterator Ver = (*this)[Targ].InstVerIter(*this);
                    if (Now == true) {
                        Ver = Targ.CurrentVer();
                    }

                    if (Ver.end() == false)
                    {
                        char buffer[1024];
                        if (Now == true) {
                            sprintf(buffer, "but %s is installed", Ver.VerStr());
                        } else {
                            sprintf(buffer, "but %s is to be installed", Ver.VerStr());
                        }

                        out << buffer;
                    } else {
                        if ((*this)[Targ].CandidateVerIter(*this).end() == true) {
                            if (Targ->ProvidesList == 0) {
                                out << "but it is not installable";
                            } else {
                                out << "but it is a virtual package";
                            }
                        } else {
                            if (Now) {
                                out << "but it is not installed";
                            } else {
                                out << "but it is not going to be installed";
                            }
                        }
                    }
                }

                if (Start != End) {
                    out << " or";
                }
                out << std::endl;

                if (Start == End){
                    break;
                }
                Start++;
            }
        }
    }
    pk_backend_job_error_code(m_job,
                              error,
                              "%s",
                              toUtf8(out.str().c_str()));
}

void AptCacheFile::buildPkgRecords()
{
    if (m_packageRecords) {
        return;
    }

    // Create the text record parser
    m_packageRecords = new pkgRecords(*this);
}

bool AptCacheFile::doAutomaticRemove()
{
    pkgAutoremove(*getDCache());

    // Now see if we destroyed anything
    if ((*this)->BrokenCount() != 0) {
        cout << "Hmm, seems like the AutoRemover destroyed something which really\n"
                "shouldn't happen. Please file a bug report against apt." << endl;
        // TODO call show_broken
        //       ShowBroken(c1out,cache,false);
        return _error->Error("Internal Error, AutoRemover broke stuff");
    }

    return true;
}

bool AptCacheFile::isRemovingEssentialPackages()
{
    string List;
    bool *Added = new bool[(*this)->Head().PackageCount];
    for (unsigned int I = 0; I != (*this)->Head().PackageCount; ++I) {
        Added[I] = false;
    }

    for (pkgCache::PkgIterator I = (*this)->PkgBegin(); ! I.end(); ++I) {
        if ((I->Flags & pkgCache::Flag::Essential) != pkgCache::Flag::Essential &&
                (I->Flags & pkgCache::Flag::Important) != pkgCache::Flag::Important) {
            continue;
        }

        if ((*this)[I].Delete() == true) {
            bool is_obsoleted = false;

            /* Following code fragment should be similar to pkgDistUpgrade's one */
            for (pkgCache::DepIterator D = I.RevDependsList(); not D.end(); ++D)
            {
                if ((D->Type == pkgCache::Dep::Obsoletes)
                    && ((*this)[D.ParentPkg()].CandidateVer != nullptr)
                    && (*this)[D.ParentPkg()].CandidateVerIter(*this).Downloadable()
                    && ((pkgCache::Version*)D.ParentVer() == (*this)[D.ParentPkg()].CandidateVer)
                    && (*this)->VS().CheckDep(I.CurrentVer().VerStr(), D)
                    && ((*this)->GetPkgPriority(D.ParentPkg()) >= (*this)->GetPkgPriority(I)))
                {
                    is_obsoleted = true;
                    break;
                }
            }

            if ((Added[I->ID] == false) && (is_obsoleted == false)) {
                Added[I->ID] = true;
                List += string(I.Name()) + " ";
            }
        }

        if (I->CurrentVer == 0) {
            continue;
        }

        // Print out any essential package depenendents that are to be removed
        for (pkgCache::DepIterator D = I.CurrentVer().DependsList(); D.end() == false; ++D) {
            // Skip everything but depends
            if (D->Type != pkgCache::Dep::PreDepends &&
                    D->Type != pkgCache::Dep::Depends){
                continue;
            }

            pkgCache::PkgIterator P = D.SmartTargetPkg();
            if ((*this)[P].Delete() == true)
            {
                bool is_obsoleted = false;

                /* Following code fragment should be similar to pkgDistUpgrade's one */
                for (pkgCache::DepIterator Dep2 = P.RevDependsList(); not Dep2.end(); ++Dep2)
                {
                    if ((Dep2->Type == pkgCache::Dep::Obsoletes)
                        && ((*this)[Dep2.ParentPkg()].CandidateVer != nullptr)
                        && (*this)[Dep2.ParentPkg()].CandidateVerIter(*this).Downloadable()
                        && ((pkgCache::Version*)Dep2.ParentVer() == (*this)[Dep2.ParentPkg()].CandidateVer)
                        && (*this)->VS().CheckDep(P.CurrentVer().VerStr(), Dep2)
                        && ((*this)->GetPkgPriority(Dep2.ParentPkg()) >= (*this)->GetPkgPriority(P)))
                    {
                        is_obsoleted = true;
                        break;
                    }
                }

                if ((Added[P->ID] == true) || (is_obsoleted == true)){
                    continue;
                }
                Added[P->ID] = true;

                char S[300];
                snprintf(S, sizeof(S), "%s (due to %s) ", P.Name(), I.Name());
                List += S;
            }
        }
    }

    delete [] Added;
    if (!List.empty()) {
        pk_backend_job_error_code(m_job,
                                  PK_ERROR_ENUM_CANNOT_REMOVE_SYSTEM_PACKAGE,
                                  "WARNING: You are trying to remove the following essential packages: %s",
                                  List.c_str());
        return true;
    }

    return false;
}

PkgInfo AptCacheFile::resolvePkgID(const gchar *packageId)
{
    g_auto(GStrv) parts = nullptr;
    pkgCache::PkgIterator pkg;

    parts = pk_package_id_split(packageId);
    pkg = (*this)->FindPkg(parts[PK_PACKAGE_ID_NAME]);

    // Ignore packages that could not be found or that exist only due to dependencies.
    if (pkg.end() || (pkg.VersionList().end() && pkg.ProvidesList().end()))
        return PkgInfo(pkgCache::VerIterator());

    // check if any intended action was encoded in this package-ID
    auto piAction = PkgAction::NONE;
    if (g_str_has_prefix(parts[PK_PACKAGE_ID_DATA], "+auto:"))
            piAction = PkgAction::INSTALL_AUTO;
    else if (g_str_has_prefix(parts[PK_PACKAGE_ID_DATA], "+manual:"))
        piAction = PkgAction::INSTALL_MANUAL;

    const pkgCache::VerIterator &ver = findVer(pkg);
    // check to see if the provided package isn't virtual too
    if (ver.end() == false &&
            strcmp(ver.VerStr(), parts[PK_PACKAGE_ID_VERSION]) == 0)
        return PkgInfo(ver, piAction);

    // check to see if the provided package isn't virtual too
    // also iterate through all available past versions
    for (auto candidateVer = findCandidateVer(pkg); !candidateVer.end(); candidateVer++) {
        if (strcmp(candidateVer.VerStr(), parts[PK_PACKAGE_ID_VERSION]) == 0)
            return PkgInfo(candidateVer, piAction);
    }

    return PkgInfo(ver, piAction);
}

gchar *AptCacheFile::buildPackageId(const pkgCache::VerIterator &ver)
{
    string data;
    pkgCache::VerFileIterator vf = ver.FileList();
    const pkgCache::PkgIterator &pkg = ver.ParentPkg();
    if (pkg->CurrentState == pkgCache::State::Installed && pkg.CurrentVer() == ver) {
        // when a package is installed, the data part of a package-id is "installed:<repo-id>"
        data = "installed:" + utilBuildPackageOriginId(vf);
    } else {
        data = utilBuildPackageOriginId(vf);
    }

    return pk_package_id_build(ver.ParentPkg().Name(),
                               ver.VerStr(),
                               ver.Arch(),
                               data.c_str());
}

pkgCache::VerIterator AptCacheFile::findVer(const pkgCache::PkgIterator &pkg)
{
    // if the package is installed return the current version
    if (!pkg.CurrentVer().end()) {
        return pkg.CurrentVer();
    }

    // Else get the candidate version iterator
    const pkgCache::VerIterator &candidateVer = findCandidateVer(pkg);
    if (!candidateVer.end()) {
        return candidateVer;
    }

    // return the version list as a last resource
    return pkg.VersionList();
}

pkgCache::VerIterator AptCacheFile::findCandidateVer(const pkgCache::PkgIterator &pkg)
{
    // get the candidate version iterator
    return (*this)[pkg].CandidateVerIter(*this);
}

std::string AptCacheFile::getShortDescription(const pkgCache::VerIterator &ver)
{
    if (ver.end() || ver.FileList().end() || GetPkgRecords() == 0) {
        return string();
    }

    return m_packageRecords->Lookup(ver.FileList()).ShortDesc();
}

std::string AptCacheFile::getLongDescription(const pkgCache::VerIterator &ver)
{
    if (ver.end() || ver.FileList().end() || GetPkgRecords() == 0) {
        return string();
    }

    return m_packageRecords->Lookup(ver.FileList()).LongDesc();
}

std::string AptCacheFile::getLongDescriptionParsed(const pkgCache::VerIterator &ver)
{
    return debParser(getLongDescription(ver));
}

bool AptCacheFile::tryToInstall(pkgProblemResolver &Fix,
                                const PkgInfo &pki,
                                bool autoInst,
                                bool preserveAuto,
                                bool fixBroken)
{
    // attempt to fix broken packages, if requested
    if (fixBroken) {
        if (!CheckDeps(false)) {
            pk_backend_job_error_code(m_job,
                                  PK_ERROR_ENUM_INTERNAL_ERROR,
                                  "Unable to resolve broken packages. Please attempt to resolve this manually, or try "
                                  "`sudo apt -f install`.");
            return false;
        }
    }

    pkgCache::PkgIterator Pkg = pki.ver.ParentPkg();

    // Check if there is something at all to install
    GetDepCache()->SetCandidateVersion(pki.ver);
    pkgDepCache::StateCache &State = (*this)[Pkg];

    if (State.CandidateVer == 0) {
        pk_backend_job_error_code(m_job,
                                  PK_ERROR_ENUM_DEP_RESOLUTION_FAILED,
                                  "Package %s is virtual and has no installation candidate",
                                  Pkg.Name());
        return false;
    }

    // Always install as "automatic" or "manual" if the package is explicitly set to
    // either of the modes (because it may have been resolved in a previous transaction
    // to be marked as automatic, for example in updates)
    // If the package indicates no explicit preference we keep the current state
    // unless the package should explicitly be marked as manually installed
    // (via preserveAuto == false).
    // See https://github.com/PackageKit/PackageKit/issues/450 for details.
    bool fromUser = false;
    if (pki.action == PkgAction::INSTALL_AUTO)
        fromUser = false;
    else if (pki.action == PkgAction::INSTALL_MANUAL)
        fromUser = true;
    else
        fromUser = preserveAuto ? !(State.Flags & pkgCache::Flag::Auto) : true;

    // FIXME: this is ignoring the return value. OTOH the return value means little to us
    //   since we run markinstall twice, once without autoinst and once with.
    //   We probably should change the return value behavior and have the callee decide whether to
    //   error out or call us again with autoinst. This however is further complicated by us
    //   having protected, so we'd have to lift protection before this?
    GetDepCache()->MarkInstall(Pkg, (fromUser ? pkgDepCache::AutoMarkFlag::Manual : pkgDepCache::AutoMarkFlag::Auto), autoInst, 0);
    // Protect against further resolver changes.
    Fix.Clear(Pkg);
    Fix.Protect(Pkg);

    return true;
}

void AptCacheFile::tryToRemove(pkgProblemResolver &Fix,
                               const PkgInfo &pki)
{
    pkgCache::PkgIterator Pkg = pki.ver.ParentPkg();

    // The package is not installed
    if (Pkg->CurrentVer == 0) {
        Fix.Clear(Pkg);
        Fix.Protect(Pkg);
        Fix.Remove(Pkg);

        return;
    }

    Fix.Clear(Pkg);
    Fix.Protect(Pkg);
    Fix.Remove(Pkg);
    // TODO this is false since PackageKit can't
    // tell it want's o purge
    GetDepCache()->MarkDelete(Pkg, false);
}

std::string AptCacheFile::debParser(std::string descr)
{
    // Policy page on package descriptions
    // http://www.debian.org/doc/debian-policy/ch-controlfields.html#s-f-Description
    unsigned int i;
    string::size_type nlpos=0;

    nlpos = descr.find('\n');
    // delete first line
    if (nlpos != string::npos) {
        descr.erase(0, nlpos + 2);        // del "\n " too
    }

    // avoid replacing '\n' for a ' ' after a '.\n' is found
    bool removedFullStop = false;
    while (nlpos < descr.length()) {
        // find the new line position
        nlpos = descr.find('\n', nlpos);
        if (nlpos == string::npos) {
            // if it could not find the new line
            // get out of the loop
            break;
        }

        i = nlpos;
        // erase the char after '\n' which is always " "
        descr.erase(++i, 1);

        // remove lines likes this: " .", making it a \n
        if (descr[i] == '.') {
            descr.erase(i, 1);
            nlpos = i;
            // don't permit the next round to replace a '\n' to a ' '
            removedFullStop = true;
            continue;
        } else if (descr[i] != ' ' && removedFullStop == false) {
            // it's not a line to be verbatim displayed
            // So it's a paragraph let's replace '\n' with a ' '
            // replace new line with " "
            descr.replace(nlpos, 1, " ");
        }

        removedFullStop = false;
        nlpos++;
    }

    return descr;
}

OpPackageKitProgress::OpPackageKitProgress(PkBackendJob *job) :
    m_job(job)
{
    // Set PackageKit status
    pk_backend_job_set_status(m_job, PK_STATUS_ENUM_LOADING_CACHE);
}

OpPackageKitProgress::~OpPackageKitProgress()
{
    Done();
}

void OpPackageKitProgress::Done()
{
    pk_backend_job_set_percentage(m_job, 100);
}

void OpPackageKitProgress::Update()
{
    if (CheckChange() == false) {
        // No change has happened skip
        return;
    }

    // Set the new percent
    pk_backend_job_set_percentage(m_job, static_cast<unsigned int>(Percent));
}
