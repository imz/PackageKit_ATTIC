#ifndef RPM_FILE_H
#define RPM_FILE_H

#include <rpm/rpmtag.h>
#include <rpm/header.h>
#include <string>
#include <vector>

class RpmFile
{
public:
    RpmFile(const std::string& filename);
    ~RpmFile();
    bool isValid() const;

    std::string packageName() const;
    std::string sourcePackage() const;
    std::string version() const;
    std::string fullVersion() const;
    std::string architecture() const;
    std::string summary() const;
    std::string description() const;
    std::string conflicts() const;
    std::vector<std::string> files() const;

    bool check();
    std::string errorMsg() const;

private:
    std::string getEntry(rpmTagVal tag) const;

    Header m_hdr = nullptr;
    std::string m_errorMsg;
    std::vector<std::string> m_files;
    bool m_isValid = false;
};

#endif // RPM_FILE_H
