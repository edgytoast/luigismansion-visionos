import Foundation

/// The environment variables that drive the launcher's headless Simulator runs
/// (SIMCTL_CHILD_LMVR_TEST_…). They exist only in Simulator builds: an app installed on a headset
/// ignores them. Set but empty counts as not set, so a test script can pass every hook and leave the
/// ones it doesn't need blank. (LMVR_AUTO_PLAY and LMVR_IMMERSION read the environment directly,
/// so devicectl launches can use them too.)
///
///   LMVR_TEST_TAB=play|controls|ports|about            the tab to open on
///   LMVR_TEST_SHEET=manage|advanced|credits|diagnostics|port:<index id>
///   LMVR_TEST_INPUTS=hands:denied,sense:R,gamepad:none  what InputMonitor reports
///   LMVR_TEST_CONTROLS=hands|sense|gamepad[,window][,page=<n>]
///   LMVR_TEST_PAD=none|xbox                            the gamepad drawing, as if none (an Xbox kind) were connected
///   LMVR_TEST_FEED=<host path>  LMVR_TEST_OFFLINE=1     the Ports tab's feed, or no network
///   LMVR_TEST_IMPORT=<host path>                       imports that file as if picked
///   LMVR_TEST_PAUSED=resume@<s>|quit@<s>               presses Resume or Quit after a Crown pause
///   LMVR_TEST_CROWN=<s>                                closes the space that long after Play, as the Crown does
enum TestHooks {
    static func value(_ name: String) -> String? {
        #if targetEnvironment(simulator)
        ProcessInfo.processInfo.environment[name].flatMap { $0.isEmpty ? nil : $0 }
        #else
        nil
        #endif
    }
}
