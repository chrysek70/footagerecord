import AppKit

// Code-drawn utility icon. No downloaded assets or font dependencies.
let output = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
for size in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let pixels = size * scale
        let image = NSImage(size: NSSize(width: pixels, height: pixels))
        image.lockFocus()
        let bounds = NSRect(x: 0, y: 0, width: pixels, height: pixels)
        let unit = Double(pixels)
        NSColor(calibratedWhite: 0.14, alpha: 1).setFill()
        NSBezierPath(roundedRect: bounds.insetBy(dx: unit * 0.07, dy: unit * 0.07),
                     xRadius: unit * 0.2, yRadius: unit * 0.2).fill()
        NSColor(calibratedRed: 0.94, green: 0.27, blue: 0.29, alpha: 1).setStroke()
        let ring = NSBezierPath(ovalIn: bounds.insetBy(dx: unit * 0.25, dy: unit * 0.25))
        ring.lineWidth = unit * 0.045
        ring.stroke()
        NSColor(calibratedRed: 0.94, green: 0.27, blue: 0.29, alpha: 1).setFill()
        NSBezierPath(ovalIn: bounds.insetBy(dx: unit * 0.355, dy: unit * 0.355)).fill()
        image.unlockFocus()
        guard let tiff = image.tiffRepresentation,
              let bitmap = NSBitmapImageRep(data: tiff),
              let png = bitmap.representation(using: .png, properties: [:]) else { fatalError("Icon rendering failed") }
        let suffix = scale == 2 ? "@2x" : ""
        try png.write(to: output.appendingPathComponent("icon_\(size)x\(size)\(suffix).png"))
    }
}
