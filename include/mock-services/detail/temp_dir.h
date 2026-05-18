#ifndef MOCK_SERVICES_DETAIL_TEMP_DIR_H
#define MOCK_SERVICES_DETAIL_TEMP_DIR_H

#include <string>

namespace mock_services {
namespace detail {

std::string unique_id();

class TempDir {
public:
    explicit TempDir(const std::string& name_prefix = "mock-services-");
    ~TempDir();

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::string& path() const;

private:
    std::string path_;

    static void erase_all(const std::string& dir);
};

}
}
#endif
