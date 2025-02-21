/* apt-utils.cpp
 *
 * Copyright (c) 2009 Daniel Nicoletti <dantti12@gmail.com>
 * Copyright (c) 2014-2022 Matthias Klumpp <mak@debian.org>
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

#include "apt-utils.h"

#include <apt-pkg/fileutl.h>
#include <apt-pkg/error.h>
#include <apt-pkg/pkgsystem.h>
#include <apt-pkg/version.h>
#include <apt-pkg/acquire-item.h>
#include <glib/gstdio.h>

#include <fstream>
#include <regex>
#include <map>

static const std::map<std::string, PkGroupEnum> groups_map = {
    { "Accessibility", PK_GROUP_ENUM_ACCESSIBILITY },
    { "Archiving/Backup", PK_GROUP_ENUM_ADMIN_TOOLS },
    { "Archiving/Cd burning", PK_GROUP_ENUM_ACCESSORIES },
    { "Archiving/Compression", PK_GROUP_ENUM_ACCESSORIES },
    { "Archiving/Other", PK_GROUP_ENUM_ACCESSORIES },
    { "Books/Computer books", PK_GROUP_ENUM_DOCUMENTATION },
    { "Books/Faqs", PK_GROUP_ENUM_DOCUMENTATION },
    { "Books/Howtos", PK_GROUP_ENUM_DOCUMENTATION },
    { "Books/Literature", PK_GROUP_ENUM_EDUCATION },
    { "Books/Other", PK_GROUP_ENUM_EDUCATION },
    { "Communications", PK_GROUP_ENUM_COMMUNICATION },
    { "Databases", PK_GROUP_ENUM_OTHER },
    { "Development/C", PK_GROUP_ENUM_PROGRAMMING },
    { "Development/C++", PK_GROUP_ENUM_PROGRAMMING },
    { "Development/Databases", PK_GROUP_ENUM_PROGRAMMING },
    { "Development/Debug", PK_GROUP_ENUM_PROGRAMMING },
    { "Development/Debuggers", PK_GROUP_ENUM_PROGRAMMING },
    { "Development/Documentation", PK_GROUP_ENUM_DOCUMENTATION },
    { "Development/Erlang", PK_GROUP_ENUM_PROGRAMMING },
    { "Development/Functional", PK_GROUP_ENUM_PROGRAMMING },
    { "Development/GNOME and GTK+", PK_GROUP_ENUM_PROGRAMMING },
    { "Development/Haskell", PK_GROUP_ENUM_PROGRAMMING },
    { "Development/Java", PK_GROUP_ENUM_PROGRAMMING },
    { "Development/KDE and QT", PK_GROUP_ENUM_PROGRAMMING },
    { "Development/Kernel", PK_GROUP_ENUM_PROGRAMMING },
    { "Development/Lisp", PK_GROUP_ENUM_PROGRAMMING },
    { "Development/ML", PK_GROUP_ENUM_PROGRAMMING },
    { "Development/Objective-C", PK_GROUP_ENUM_PROGRAMMING },
    { "Development/Other", PK_GROUP_ENUM_PROGRAMMING },
    { "Development/Perl", PK_GROUP_ENUM_PROGRAMMING },
    { "Development/Python", PK_GROUP_ENUM_PROGRAMMING },
    { "Development/Python3", PK_GROUP_ENUM_PROGRAMMING },
    { "Development/Ruby", PK_GROUP_ENUM_PROGRAMMING },
    { "Development/Scheme", PK_GROUP_ENUM_PROGRAMMING },
    { "Development/Tcl", PK_GROUP_ENUM_PROGRAMMING },
    { "Development/Tools", PK_GROUP_ENUM_PROGRAMMING },
    { "Documentation", PK_GROUP_ENUM_DOCUMENTATION },
    { "Editors", PK_GROUP_ENUM_PUBLISHING },
    { "Education", PK_GROUP_ENUM_EDUCATION },
    { "Emulators", PK_GROUP_ENUM_SYSTEM },
    { "Engineering", PK_GROUP_ENUM_ELECTRONICS },
    { "File tools", PK_GROUP_ENUM_ACCESSORIES },
    { "Games/Adventure", PK_GROUP_ENUM_GAMES },
    { "Games/Arcade", PK_GROUP_ENUM_GAMES },
    { "Games/Boards", PK_GROUP_ENUM_GAMES },
    { "Games/Cards", PK_GROUP_ENUM_GAMES },
    { "Games/Educational", PK_GROUP_ENUM_GAMES },
    { "Games/Other", PK_GROUP_ENUM_GAMES },
    { "Games/Puzzles", PK_GROUP_ENUM_GAMES },
    { "Games/Sports", PK_GROUP_ENUM_GAMES },
    { "Games/Strategy", PK_GROUP_ENUM_GAMES },
    { "Graphical desktop/Enlightenment", PK_GROUP_ENUM_DESKTOP_OTHER },
    { "Graphical desktop/FVWM based", PK_GROUP_ENUM_DESKTOP_OTHER },
    { "Graphical desktop/GNOME", PK_GROUP_ENUM_DESKTOP_GNOME },
    { "Graphical desktop/GNUstep", PK_GROUP_ENUM_DESKTOP_OTHER },
    { "Graphical desktop/Icewm", PK_GROUP_ENUM_DESKTOP_OTHER },
    { "Graphical desktop/KDE", PK_GROUP_ENUM_DESKTOP_KDE },
    { "Graphical desktop/MATE", PK_GROUP_ENUM_DESKTOP_OTHER },
    { "Graphical desktop/Motif", PK_GROUP_ENUM_DESKTOP_OTHER },
    { "Graphical desktop/Other", PK_GROUP_ENUM_DESKTOP_OTHER },
    { "Graphical desktop/Rox", PK_GROUP_ENUM_DESKTOP_OTHER },
    { "Graphical desktop/Sawfish", PK_GROUP_ENUM_DESKTOP_OTHER },
    { "Graphical desktop/Sugar", PK_GROUP_ENUM_DESKTOP_OTHER },
    { "Graphical desktop/Window Maker", PK_GROUP_ENUM_DESKTOP_OTHER },
    { "Graphical desktop/XFce", PK_GROUP_ENUM_DESKTOP_XFCE },
    { "Graphics", PK_GROUP_ENUM_GRAPHICS },
    { "Monitoring", PK_GROUP_ENUM_ACCESSORIES },
    { "Networking/Chat", PK_GROUP_ENUM_COMMUNICATION },
    { "Networking/DNS", PK_GROUP_ENUM_NETWORK },
    { "Networking/File transfer", PK_GROUP_ENUM_NETWORK },
    { "Networking/FTN", PK_GROUP_ENUM_NETWORK },
    { "Networking/IRC", PK_GROUP_ENUM_COMMUNICATION },
    { "Networking/Instant messaging", PK_GROUP_ENUM_COMMUNICATION },
    { "Networking/Mail", PK_GROUP_ENUM_INTERNET },
    { "Networking/News", PK_GROUP_ENUM_INTERNET },
    { "Networking/Other", PK_GROUP_ENUM_NETWORK },
    { "Networking/Remote access", PK_GROUP_ENUM_NETWORK },
    { "Networking/WWW", PK_GROUP_ENUM_INTERNET },
    { "Office", PK_GROUP_ENUM_OFFICE },
    { "Other", PK_GROUP_ENUM_OTHER },
    { "Publishing", PK_GROUP_ENUM_PUBLISHING },
    { "Sciences/Astronomy", PK_GROUP_ENUM_SCIENCE },
    { "Sciences/Biology", PK_GROUP_ENUM_SCIENCE },
    { "Sciences/Chemistry", PK_GROUP_ENUM_SCIENCE },
    { "Sciences/Computer science", PK_GROUP_ENUM_SCIENCE },
    { "Sciences/Geosciences", PK_GROUP_ENUM_SCIENCE },
    { "Sciences/Mathematics", PK_GROUP_ENUM_SCIENCE },
    { "Sciences/Medicine", PK_GROUP_ENUM_SCIENCE },
    { "Sciences/Other", PK_GROUP_ENUM_SCIENCE },
    { "Sciences/Physics", PK_GROUP_ENUM_SCIENCE },
    { "Security/Antivirus", PK_GROUP_ENUM_SCIENCE },
    { "Security/Networking", PK_GROUP_ENUM_SCIENCE },
    { "Shells", PK_GROUP_ENUM_SYSTEM },
    { "Sound", PK_GROUP_ENUM_MULTIMEDIA },
    { "System/Base", PK_GROUP_ENUM_SYSTEM },
    { "System/Configuration/Boot and Init", PK_GROUP_ENUM_SYSTEM },
    { "System/Configuration/Hardware", PK_GROUP_ENUM_ADMIN_TOOLS },
    { "System/Configuration/Networking", PK_GROUP_ENUM_NETWORK },
    { "System/Configuration/Other", PK_GROUP_ENUM_SYSTEM },
    { "System/Configuration/Packaging", PK_GROUP_ENUM_ADMIN_TOOLS },
    { "System/Configuration/Printing", PK_GROUP_ENUM_SYSTEM },
    { "System/Fonts/Console", PK_GROUP_ENUM_FONTS },
    { "System/Fonts/True type", PK_GROUP_ENUM_FONTS },
    { "System/Fonts/Type1", PK_GROUP_ENUM_FONTS },
    { "System/Fonts/X11 bitmap", PK_GROUP_ENUM_FONTS },
    { "System/Internationalization", PK_GROUP_ENUM_LOCALIZATION },
    { "System/Kernel and hardware", PK_GROUP_ENUM_ADMIN_TOOLS },
    { "System/Libraries", PK_GROUP_ENUM_SYSTEM },
    { "System/Legacy libraries", PK_GROUP_ENUM_LEGACY },
    { "System/Servers", PK_GROUP_ENUM_SERVERS },
    { "System/Servers/ZProducts", PK_GROUP_ENUM_UNKNOWN },
    { "System/X11", PK_GROUP_ENUM_DESKTOP_OTHER },
    { "System/XFree86", PK_GROUP_ENUM_DESKTOP_OTHER },
    { "Terminals", PK_GROUP_ENUM_DESKTOP_OTHER },
    { "Text tools", PK_GROUP_ENUM_PUBLISHING },
    { "Toys", PK_GROUP_ENUM_GAMES },
    { "Video", PK_GROUP_ENUM_MULTIMEDIA },
};

PkGroupEnum get_enum_group(string group)
{
    auto iter = groups_map.find(group);
    if (iter != groups_map.end())
    {
        return iter->second;
    }
    else
    {
        return PK_GROUP_ENUM_UNKNOWN;
    }
}

void changesFromChangelog(GMatchInfo *changelog_match_info, GPtrArray *changelogs)
{
    if (!g_match_info_matches(changelog_match_info)) {
        return;
    }

    auto *matchInfo = g_match_info_fetch(changelog_match_info, 0);
    g_ptr_array_add(changelogs, (gpointer) matchInfo);

    g_match_info_next(changelog_match_info, NULL);

    return changesFromChangelog(changelog_match_info, changelogs);
}

GPtrArray* getChangelogChanges(AptCacheFile &CacheFile,
                         pkgCache::VerIterator &Ver)
{

    GRegex *changelog_re;
    GMatchInfo *changelog_match_info;
    auto *changelogs = g_ptr_array_new();

    changelog_re = g_regex_new("\\*\\s[^\\n]*(?:\\n(?!\\*\\s).*)*",
                           G_REGEX_MULTILINE,
                           GRegexMatchFlags{},
                           NULL);

    pkgCache::VerFileIterator Vf = Ver.FileList();
    for (; Vf.end() == false; Vf++) {
        if ((Vf.File()->Flags & pkgCache::Flag::NotSource) == 0) {
            break;
        }
    }

    if (Vf.end() == true) {
        Vf = Ver.FileList();
    }

    pkgRecords Recs(*CacheFile);
    pkgRecords::Parser &P = Recs.Lookup(Vf);
    auto common_changelog = P.Changelog();

    g_regex_match (changelog_re, common_changelog.c_str(), GRegexMatchFlags{}, &changelog_match_info);
    changesFromChangelog(changelog_match_info, changelogs);

    g_match_info_free(changelog_match_info);
    g_regex_unref(changelog_re);

    return changelogs;
}

string fetchChangelogData(AptCacheFile &CacheFile,
                          pkgAcquire &Fetcher,
                          pkgCache::VerIterator Ver,
                          pkgCache::VerIterator currver,
                          string *update_text,
                          string *updated,
                          string *issued)
{
    string changelog;

    auto *changelogs = getChangelogChanges(CacheFile, Ver);
    for (guint i = 0; i < changelogs->len; ++i) {
        auto *changelog_entry = (gchar *)g_ptr_array_index(changelogs, i);
        changelog.append(changelog_entry);
        changelog.append("\n");
    }

    if (changelog.empty()) {
        return changelog;
    }

    GRegex *content_re;
    GMatchInfo *content_match_info;

    content_re = g_regex_new("\\*\\s+(?<date>.*\\d{4})\\s+(?<packager>.*)\\s+(?<mail><.*>)\\s+(?<version>.*?)\\b\\n(?<content>.*)",
                             G_REGEX_DOTALL,
                             GRegexMatchFlags{},
                             NULL);

    bool isEnd = false;
    for (guint i = 0; i < changelogs->len && !isEnd; ++i) {
        auto *changelog_entry = (gchar *)g_ptr_array_index(changelogs, i);

        g_regex_match(content_re, changelog_entry, GRegexMatchFlags{}, &content_match_info);
        if (g_match_info_matches(content_match_info)) {
            auto *pkg_date = g_match_info_fetch_named(content_match_info, "date");
            auto *pkg_packager = g_match_info_fetch_named(content_match_info, "packager");
            auto *pkg_mail = g_match_info_fetch_named(content_match_info, "mail");
            auto *pkg_version = g_match_info_fetch_named(content_match_info, "version");
            auto *pkg_content = g_match_info_fetch_named(content_match_info, "content");

            auto *formatChangelog = g_strdup_printf("\n\n == %s ==\n"
                                                    "%s — %s %s %s", pkg_version, pkg_content,
                                                    pkg_packager, pkg_mail, pkg_date);

            update_text->append(formatChangelog);
            issued->append(pkg_date);

            if (_system != 0 && _system->VS->DoCmpVersion(pkg_version, pkg_version+strlen(pkg_version),
                currver.VerStr(), pkg_version+strlen(currver.VerStr())) <= 0) {

                isEnd = true;
            }

            g_free(pkg_date);
            g_free(pkg_packager);
            g_free(pkg_mail);
            g_free(pkg_version);
            g_free(pkg_content);
        }

        g_match_info_free(content_match_info);
    }

    g_ptr_array_free(changelogs, true);
    g_regex_unref(content_re);

#if 0
    pkgAcqChangelog *c = new pkgAcqChangelog(&Fetcher, Ver);

    // try downloading it, if that fails, try third-party-changelogs location
    // FIXME: Fetcher.Run() is "Continue" even if I get a 404?!?
    Fetcher.Run();

    // error
    pkgRecords Recs(CacheFile);
    pkgCache::PkgIterator Pkg = Ver.ParentPkg();
    pkgRecords::Parser &rec=Recs.Lookup(Ver.FileList());
    string srcpkg = rec.SourcePkg().empty() ? Pkg.Name() : rec.SourcePkg();
#endif

#if 0
    // return empty string if we don't have a file to read
    if (!FileExists(c->DestFile)) {
        return changelog;
    }

    if (_error->PendingError()) {
        return changelog;
    }

    ifstream in(c->DestFile.c_str());
    string line;
    g_autoptr(GRegex) regexVer = NULL;
    regexVer = g_regex_new("(?'source'.+) \\((?'version'.*)\\) "
                           "(?'dist'.+); urgency=(?'urgency'.+)",
                           G_REGEX_CASELESS,
                           G_REGEX_MATCH_ANCHORED,
                           0);
    g_autoptr(GRegex) regexDate = NULL;
    regexDate = g_regex_new("^ -- (?'maintainer'.+) (?'mail'<.+>)  (?'date'.+)$",
                            G_REGEX_CASELESS,
                            G_REGEX_MATCH_ANCHORED,
                            0);

    changelog = "";
    while (getline(in, line)) {
        // we don't want the additional whitespace, because it can confuse
        // some markdown parsers used by client tools
        if (starts_with(line, "  "))
            line.erase(0,1);
        // no need to free str later, it is allocated in a static buffer
        const char *str = toUtf8(line.c_str());
        if (strcmp(str, "") == 0) {
            changelog.append("\n");
            continue;
        }

        if (starts_with(str, srcpkg.c_str())) {
            // Check to see if the the text isn't about the current package,
            // otherwise add a == version ==
            GMatchInfo *match_info;
            if (g_regex_match(regexVer, str, G_REGEX_MATCH_ANCHORED, &match_info)) {
                gchar *version;
                version = g_match_info_fetch_named(match_info, "version");

                // Compare if the current version is shown in the changelog, to not
                // display old changelog information
                if (_system != 0  &&
                        _system->VS->DoCmpVersion(version, version + strlen(version),
                                                  currver.SourceVerStr(),
                                                  currver.SourceVerStr() + strlen(currver.SourceVerStr())) <= 0) {
                    g_free (version);
                    break;
                } else {
                    if (!update_text->empty()) {
                        update_text->append("\n\n");
                    }
                    update_text->append(" == ");
                    update_text->append(version);
                    update_text->append(" ==");
                    g_free (version);
                }
            }
            g_match_info_free (match_info);
        } else if (starts_with(str, " ")) {
            // update descritption
            update_text->append("\n");
            update_text->append(str);
        } else if (starts_with(str, " --")) {
            // Parse the text to know when the update was issued,
            // and when it got updated
            GMatchInfo *match_info;
            if (g_regex_match(regexDate, str, G_REGEX_MATCH_ANCHORED, &match_info)) {
                g_autoptr(GDateTime) dateTime = NULL;
                g_autofree gchar *date = NULL;
                date = g_match_info_fetch_named(match_info, "date");
                time_t time;
                g_warn_if_fail(RFC1123StrToTime(date, time));
                dateTime = g_date_time_new_from_unix_local(time);

                *issued = g_date_time_format_iso8601(dateTime);
                if (updated->empty()) {
                    *updated = g_date_time_format_iso8601(dateTime);
                }
            }
            g_match_info_free(match_info);
        }

        changelog.append(str);
        changelog.append("\n");
    }
#endif

    changelog.erase(changelog.find_last_not_of(" \t\n") + 1);
    return changelog;
}

GPtrArray* getCVEUrls(const string &changelog)
{
    GPtrArray *cve_urls = g_ptr_array_new();

    // Regular expression to find cve references
    GRegex *regex;
    GMatchInfo *match_info;
    regex = g_regex_new("CVE-\\d{4}-\\d{4,}",
                        G_REGEX_CASELESS,
                        G_REGEX_MATCH_NEWLINE_ANY,
                        0);
    g_regex_match (regex, changelog.c_str(), G_REGEX_MATCH_NEWLINE_ANY, &match_info);
    while (g_match_info_matches(match_info)) {
        gchar *cve = g_match_info_fetch (match_info, 0);
        gchar *cveLink;

        cveLink = g_strdup_printf("https://web.nvd.nist.gov/view/vuln/detail?vulnId=%s", cve);
        g_ptr_array_add(cve_urls, (gpointer) cveLink);

        g_free(cve);
        g_match_info_next(match_info, NULL);
    }
    g_match_info_free(match_info);
    g_regex_unref(regex);

    // NULL terminate
    g_ptr_array_add(cve_urls, NULL);

    return cve_urls;
}

GPtrArray* getBugzillaUrls(const string &changelog)
{
    GPtrArray *bugzilla_urls = g_ptr_array_new();

    // Matches ALT Linux bugs
    GRegex *regex,
           *bug_num_re;
    GMatchInfo *match_info;
    regex = g_regex_new("\\((?:closes:|alt(?:\\s+bug)?)\\s+(?'bugs'(?:,?#?[\\d\\s]+)+)\\)",

                        G_REGEX_CASELESS,
                        G_REGEX_MATCH_NEWLINE_ANY,
                        0);
    bug_num_re = g_regex_new("(?'bug'\\d+)",
                             G_REGEX_OPTIMIZE,
                             GRegexMatchFlags{},
                             0);

    g_regex_match (regex, changelog.c_str(), G_REGEX_MATCH_NEWLINE_ANY, &match_info);
    while (g_match_info_matches(match_info)) {
        gchar *bugs = g_match_info_fetch_named(match_info, "bugs");

        GMatchInfo *bug_num_minfo;
        g_regex_match(bug_num_re, bugs, GRegexMatchFlags{}, &bug_num_minfo);
        while (g_match_info_matches(bug_num_minfo)) {
            gchar *bug = g_match_info_fetch_named(bug_num_minfo, "bug");
            gchar *bugLink;

            bugLink = g_strdup_printf("https://bugzilla.altlinux.org/%s", bug);
            g_ptr_array_add(bugzilla_urls, (gpointer) bugLink);

            g_free(bug);
            g_match_info_next(bug_num_minfo, NULL);
        }

        g_match_info_free(bug_num_minfo);

        g_free(bugs);
        g_match_info_next(match_info, NULL);
    }

    g_regex_unref(bug_num_re);

    g_match_info_free(match_info);
    g_regex_unref(regex);

#if 0
    // Debian bugs
    // Regular expressions to detect bug numbers in changelogs according to the
    // Debian Policy Chapter 4.4. For details see the footnote 15:
    // https://www.debian.org/doc/debian-policy/footnotes.html#f15
    // /closes:\s*(?:bug)?\#?\s?\d+(?:,\s*(?:bug)?\#?\s?\d+)*/i
    regex = g_regex_new("closes:\\s*(?:bug)?\\#?\\s?(?'bug1'\\d+)(?:,\\s*(?:bug)?\\#?\\s?(?'bug2'\\d+))*",
                        G_REGEX_CASELESS,
                        G_REGEX_MATCH_NEWLINE_ANY,
                        0);
    g_regex_match (regex, changelog.c_str(), G_REGEX_MATCH_NEWLINE_ANY, &match_info);
    while (g_match_info_matches(match_info)) {
        gchar *bug1 = g_match_info_fetch_named(match_info, "bug1");
        gchar *bugLink1;

        bugLink1 = g_strdup_printf("https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=%s", bug1);
        g_ptr_array_add(bugzilla_urls, (gpointer) bugLink1);

        g_free(bug1);

        gchar *bug2 = g_match_info_fetch_named(match_info, "bug2");
        if (bug2 != NULL && bug2[0] != '\0') {
            gchar *bugLink2;

            bugLink2 = g_strdup_printf("https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=%s", bug2);
            g_ptr_array_add(bugzilla_urls, (gpointer) bugLink2);

            g_free(bug2);
        }

        g_match_info_next(match_info, NULL);
    }
    g_match_info_free(match_info);
    g_regex_unref(regex);
#endif

    // NULL terminate
    g_ptr_array_add(bugzilla_urls, NULL);

    return bugzilla_urls;
}

bool ends_with(const string &str, const char *end)
{
    size_t endSize = strlen(end);
    return str.size() >= endSize && (memcmp(str.data() + str.size() - endSize, end, endSize) == 0);
}

bool starts_with(const string &str, const char *start)
{
    size_t startSize = strlen(start);
    return str.size() >= startSize && (strncmp(str.data(), start, startSize) == 0);
}

bool utilRestartRequired(const string &packageName)
{
    if (starts_with(packageName, "linux-image-") ||
        starts_with(packageName, "nvidia-") ||
        packageName == "libc6" ||
        packageName == "dbus" ||
        packageName == "dbus-broker") {
        return true;
    }
    return false;
}

string utilBuildPackageOriginId(pkgCache::VerFileIterator vf)
{
    if (vf.File().Origin() == nullptr)
        return string("local");
    if (vf.File().Archive() == nullptr)
        return string("local");
    if (vf.File().Component() == nullptr)
        return string("invalid");

    // https://wiki.debian.org/DebianRepository/Format
    // Optional field indicating the origin of the repository, a single line of free form text.
    // e.g. "Debian" or "Google Inc."
    auto origin = string(vf.File().Origin());
    // The Suite field may describe the suite. A suite is a single word.
    // e.g. "jessie" or "sid"
    auto suite = string(vf.File().Archive());
    // An area within the repository. May be prefixed by parts of the path
    // following the directory beneath dists.
    // e.g. "main" or "non-free"
    // NOTE: this may need the slash stripped, currently having a slash doesn't
    //    seem a problem though. we'll allow them until otherwise indicated
    auto component = string(vf.File().Component());

    // Don't repeat the "ALT Linux" prefix both in Origin and Suite,
    // and shorten this Origin.
    static constexpr char ALT_Linux_[] = "ALT Linux ";
    static constexpr size_t len_ALT_Linux_ = sizeof ALT_Linux_ - 1;
    if ((strcasecmp(origin.c_str(), "ALT Linux Team") == 0)
        && (strncasecmp(suite.c_str(), ALT_Linux_, len_ALT_Linux_) == 0)
        && (suite.length() > len_ALT_Linux_))
    {
        suite = suite.substr(len_ALT_Linux_);
        origin = std::string(ALT_Linux_, 0, len_ALT_Linux_-1);
    }

    // Origin is defined as 'a single line of free form text'.
    // Sanitize it!
    // All space characters, control characters and punctuation get replaced
    // with underscore.
    // In particular the punctuations ':', ';' and ',' may be used as list separators
    // so we must not have them appear in our package_ids as that would cause
    // breakage down the line.
    std::transform(origin.begin(), origin.end(), origin.begin(), ::tolower);
    origin = std::regex_replace(origin, std::regex("[[:space:][:cntrl:][:punct:]]+"), "_");

    std::transform(suite.begin(), suite.end(), suite.begin(), ::tolower);
    suite = std::regex_replace(suite, std::regex("[[:space:][:cntrl:][:punct:]]+"), "_");

    std::transform(component.begin(), component.end(), component.begin(), ::tolower);
    component = std::regex_replace(component, std::regex("[[:space:][:cntrl:][:punct:]]+"), "_");

    string res = origin + "-" + suite + "-" + component;
    return res;
}

const char *toUtf8(const char *str)
{
    static __thread char *_str = NULL;
    if (str == NULL)
        return NULL;

    if (g_utf8_validate(str, -1, NULL) == true)
        return str;

    g_free(_str);
    _str = NULL;
    _str = g_locale_to_utf8(str, -1, NULL, NULL, NULL);
    return _str;
}
