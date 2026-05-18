#pragma once

#include <string>

namespace mock_services {
namespace ftp {


namespace status {
    const int file_ok              = 150;
    const int command_ok           = 200;
    const int ready                = 220;
    const int closing              = 221;
    const int transfer_complete    = 226;
    const int passive_mode         = 227;
    const int logged_in            = 230;
    const int file_deleted         = 250;
    const int path_created         = 257;
    const int user_ok              = 331;
    const int cant_open_data       = 425;
    const int syntax_error         = 501;
    const int not_implemented      = 502;
    const int login_incorrect      = 530;
    const int file_unavailable     = 550;
    const int permission_denied    = 550;
    const int file_unavailable_name = 553;
}


namespace cmd {
    extern const std::string user;
    extern const std::string pass;
    extern const std::string stor;
    extern const std::string retr;
    extern const std::string list;
    extern const std::string dele;
    extern const std::string mkd;
    extern const std::string rmd;
    extern const std::string pwd;
    extern const std::string cwd;
    extern const std::string quit;
    extern const std::string pasv;
    extern const std::string type;
    extern const std::string syst;
    extern const std::string noop;
    extern const std::string cdup;
}


static_assert(ftp::status::file_ok == 150,           "bad constant");
static_assert(ftp::status::command_ok == 200,         "bad constant");
static_assert(ftp::status::ready == 220,              "bad constant");
static_assert(ftp::status::closing == 221,            "bad constant");
static_assert(ftp::status::transfer_complete == 226,  "bad constant");
static_assert(ftp::status::passive_mode == 227,       "bad constant");
static_assert(ftp::status::logged_in == 230,          "bad constant");
static_assert(ftp::status::file_deleted == 250,       "bad constant");
static_assert(ftp::status::path_created == 257,       "bad constant");
static_assert(ftp::status::user_ok == 331,            "bad constant");
static_assert(ftp::status::cant_open_data == 425,     "bad constant");
static_assert(ftp::status::syntax_error == 501,       "bad constant");
static_assert(ftp::status::not_implemented == 502,    "bad constant");
static_assert(ftp::status::login_incorrect == 530,    "bad constant");
static_assert(ftp::status::file_unavailable == 550,   "bad constant");
static_assert(ftp::status::file_unavailable_name == 553, "bad constant");

}
}
