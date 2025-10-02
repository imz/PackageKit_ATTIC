#include "rpm-file.h"

#include <glib.h>
#include <apt-pkg/configuration.h>
#include <rpm/rpmlib.h>
#include <rpm/rpmts.h>

RpmFile::RpmFile(const std::string& filename)
{
    rpmReadConfigFiles(NULL, NULL);

    rpmts ts = rpmtsCreate();
    rpmtsSetVSFlags(ts, (rpmVSFlags)-1);

    // Open and read RPM file using native RPM library functions
    FD_t fd = Fopen(filename.c_str(), "r.fdio");
    if (fd == NULL || Ferror(fd)) {
        if (fd) Fclose(fd);
        m_errorMsg = "Failed to open RPM file";
        return;
    }

    // Read RPM header using native RPM library
    rpmRC rc = rpmReadPackageFile(ts, fd, filename.c_str(), &m_hdr);

    Fclose(fd);
    rpmtsFree(ts);

    if (rc != RPMRC_OK) {
        m_errorMsg = "Invalid RPM file format";
        return;
    }

    // Get file list from RPM
    rpmfi fi = rpmfiNew(NULL, m_hdr, RPMTAG_BASENAMES, RPMFI_NOHEADER);
    if (fi) {
        while (rpmfiNext(fi) >= 0) {
            const char *filepath = rpmfiFN(fi);
            if (filepath) {
                m_files.push_back(filepath);
            }
        }
        rpmfiFree(fi);
    }

    m_isValid = true;
}

RpmFile::~RpmFile()
{
    if (m_hdr) {
        headerFree(m_hdr);
    }
}

std::vector<std::string> RpmFile::files() const
{
    return m_files;
}

bool RpmFile::isValid() const
{
    return m_isValid;
}

std::string RpmFile::getEntry(rpmTagVal tag) const
{
    if (!m_hdr) {
        return {};
    }

    struct rpmtd_s td;
    if (!headerGet(m_hdr, tag, &td, HEADERGET_DEFAULT)) {
        switch (tag) {
        case RPMTAG_NAME:
            g_debug("No Package name field in the package");
            break;
        case RPMTAG_SOURCE:
            g_debug("No Source field in the package");
            break;
        case RPMTAG_VERSION:
            g_debug("No Version field in the package");
            break;
        case RPMTAG_ARCH:
            g_debug("No Arch field in the package");
            break;
        case RPMTAG_SUMMARY:
            g_debug("No Summary field in the package");
            break;
        case RPMTAG_DESCRIPTION:
            g_debug("No Description field in the package");
            break;
        case RPMTAG_CONFLICTNAME:
            g_debug("No Conflicts field in the package");
            break;
        case RPMTAG_LICENSE:
            g_debug("No License field in the package");
            break;
        }

        return {};
    }

    std::string result = rpmtdGetString(&td);
    rpmtdFreeData(&td);

    return result;
}

std::string RpmFile::packageName() const
{
    return getEntry(RPMTAG_NAME);
}

std::string RpmFile::sourcePackage() const
{
    return getEntry(RPMTAG_SOURCE);
}

std::string RpmFile::version() const
{
    return getEntry(RPMTAG_VERSION);
}

std::string RpmFile::fullVersion() const
{
    return version() + "-" + getEntry(RPMTAG_RELEASE);
}

std::string RpmFile::architecture() const
{
    return getEntry(RPMTAG_ARCH);
}

std::string RpmFile::conflicts() const
{
    // not implemented
    return {};
}

std::string RpmFile::license() const
{
    return getEntry(RPMTAG_LICENSE);
}

std::string RpmFile::summary() const
{
    return getEntry(RPMTAG_SUMMARY);
}

std::string RpmFile::description() const
{
    return getEntry(RPMTAG_DESCRIPTION);
}

bool RpmFile::check()
{
    // check arch
    if (architecture().empty()) {
        m_errorMsg = "No Architecture field in the package";
        return false;
    }

    g_debug("RpmFile architecture: %s", architecture().c_str());
    if (architecture().compare("all") != 0 &&
            architecture().compare(_config->Find("APT::Architecture")) != 0) {
        m_errorMsg = "Wrong architecture ";
        m_errorMsg.append(architecture());
        return false;
    }

    return true;
}

std::string RpmFile::errorMsg() const
{
    return m_errorMsg;
}
