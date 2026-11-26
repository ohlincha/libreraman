#pragma once
// `LibreRamanCLI --remote <command> ...`: talks to the running LibreRaman application through its local API
// (core/apiclient.h, gui/apiserver.h) instead of opening the instrument itself.
#include <QStringList>

namespace lr::cli {

// Exit codes of the remote commands (also documented in the usage text).
enum RemoteExit {
    kOk = 0,
    kFailed = 1,          // error reply, failed measurement
    kUsage = 2,
    kNotRunning = 3,      // no API file / cannot connect
    kDenied = 4,          // 401 wrong token, 403 remote measurements not allowed
    kBusy = 5,            // 409 a measurement is running
    kStopped = 130,       // the measurement was stopped
};

int runRemote(const QStringList& args);    // args without "--remote"
extern const char* const REMOTE_USAGE;

}  // namespace lr::cli
