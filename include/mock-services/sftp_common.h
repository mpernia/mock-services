#pragma once

#include <string>

namespace mock_services {
namespace sftp {


namespace operation {
    extern const std::string open;
    extern const std::string opendir;
    extern const std::string read;
    extern const std::string write;
    extern const std::string close;
    extern const std::string remove;
    extern const std::string mkdir;
    extern const std::string rmdir;
    extern const std::string rename;
}


namespace result {
    extern const std::string ok;
    extern const std::string denied;
    extern const std::string no_such_file;
    extern const std::string failure;
    extern const std::string no_such_path;
}

}
}
