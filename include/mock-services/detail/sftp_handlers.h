#ifndef MOCK_SERVICES_DETAIL_SFTP_HANDLERS_H
#define MOCK_SERVICES_DETAIL_SFTP_HANDLERS_H

#include <string>

#include <libssh/libssh.h>

#ifndef WITH_SERVER
#define WITH_SERVER
#endif
#include <libssh/sftp.h>

#include "mock-services/sftp_server.h"

namespace mock_services {

struct SftpHandlerServices {
    void* userdata;
    std::string (*resolve_path)(void* userdata, const std::string& root,
                                 const std::string& path, bool must_exist);
    void (*log_activity)(void* userdata, const std::string& user,
                          const std::string& op, const std::string& path,
                          const std::string& result);
};

struct SftpContext {
    sftp_session sftp;
    ssh_channel channel;
    std::string username;
    std::string root;
    sftp_permission perm;
    SftpHandlerServices* svc;

    std::string resolve_path(const std::string& path, bool must_exist) {
        return svc->resolve_path(svc->userdata, root, path, must_exist);
    }

    void log_activity(const std::string& op,
                      const std::string& path,
                      const std::string& result) {
        svc->log_activity(svc->userdata, username, op, path, result);
    }
};

void sftp_do_open(SftpContext& ctx, sftp_client_message msg);
void sftp_do_close(SftpContext& ctx, sftp_client_message msg);
void sftp_do_read(SftpContext& ctx, sftp_client_message msg);
void sftp_do_write(SftpContext& ctx, sftp_client_message msg);
void sftp_do_opendir(SftpContext& ctx, sftp_client_message msg);
void sftp_do_readdir(SftpContext& ctx, sftp_client_message msg);
void sftp_do_stat(SftpContext& ctx, sftp_client_message msg);
void sftp_do_fstat(SftpContext& ctx, sftp_client_message msg);
void sftp_do_setstat(SftpContext& ctx, sftp_client_message msg);
void sftp_do_fsetstat(SftpContext& ctx, sftp_client_message msg);
void sftp_do_mkdir(SftpContext& ctx, sftp_client_message msg);
void sftp_do_rmdir(SftpContext& ctx, sftp_client_message msg);
void sftp_do_remove(SftpContext& ctx, sftp_client_message msg);
void sftp_do_rename(SftpContext& ctx, sftp_client_message msg);
void sftp_do_realpath(SftpContext& ctx, sftp_client_message msg);

}

#endif
