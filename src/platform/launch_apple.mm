// The packaged macOS app: data folder, log and ROM panel (see launch.hpp).
#include "platform/launch.hpp"
#include "platform/crash_log.hpp"

#include <cstdio>
#include <system_error>

#include <unistd.h>

#import <AppKit/AppKit.h>

namespace conker::launch {

bool from_desktop() {
    return [[[NSBundle mainBundle] bundlePath] hasSuffix:@".app"];
}

void enter_data_folder() {
    if (!from_desktop()) {
        return;
    }
    NSURL* support = [[[NSFileManager defaultManager] URLsForDirectory:NSApplicationSupportDirectory
                                                             inDomains:NSUserDomainMask] firstObject];
    if (support == nil) {
        return;
    }
    const std::filesystem::path folder =
        std::filesystem::path(support.fileSystemRepresentation) / "ConkersBadFurDayRecompiled";
    std::error_code error;
    std::filesystem::create_directories(folder / "conker-recomp-data", error);
    std::filesystem::current_path(folder, error);
    if (error) {
        return;
    }
    crash_log::start_run(folder / "conker-recomp-data");
    const std::string log = (folder / "conker-recomp-data" / "last-run.log").string();
    std::freopen(log.c_str(), "w", stdout);
    // stderr shares stdout's open file, and so its position: a second open of the file kept its own
    // position, and stdout's lines wrote over stderr's (an OUT OF SYNC report was lost that way).
    dup2(fileno(stdout), fileno(stderr));
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
}

std::filesystem::path choose_rom() {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.title = @"Choose your Conker's Bad Fur Day ROM";
        panel.message = @"Choose your own US ROM (.z64, .n64 or .v64). A copy is kept, so this is asked once.";
        panel.prompt = @"Use ROM";
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        [NSApp activateIgnoringOtherApps:YES];
        if ([panel runModal] != NSModalResponseOK || panel.URL == nil) {
            return {};
        }
        return std::filesystem::path(panel.URL.fileSystemRepresentation);
    }
}

void warn_if_emulated() {}

}  // namespace conker::launch
