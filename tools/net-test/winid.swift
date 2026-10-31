import CoreGraphics
// Prints "<window id> <owner pid>" for every on-screen window owned by a process named ConkersBadFurDayRecompiled.
let list = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as! [[String: Any]]
for w in list {
    if ((w[kCGWindowOwnerName as String] as? String) ?? "").hasPrefix("ConkersBadFurDayRecompiled"), (w[kCGWindowLayer as String] as? Int) == 0 {
        print(w[kCGWindowNumber as String]!, w[kCGWindowOwnerPID as String]!)
    }
}
