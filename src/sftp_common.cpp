#include "mock-services/sftp_common.h"

namespace mock_services { namespace sftp { namespace operation {
const std::string open    = "OPEN";
const std::string opendir = "OPENDIR";
const std::string read    = "READ";
const std::string write   = "WRITE";
const std::string close   = "CLOSE";
const std::string remove  = "REMOVE";
const std::string mkdir   = "MKDIR";
const std::string rmdir   = "RMDIR";
const std::string rename  = "RENAME";
}}}

namespace mock_services { namespace sftp { namespace result {
const std::string ok            = "OK";
const std::string denied        = "DENIED";
const std::string no_such_file  = "NO_SUCH_FILE";
const std::string failure       = "FAILURE";
const std::string no_such_path  = "NO_SUCH_PATH";
}}}
