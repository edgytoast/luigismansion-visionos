import SwiftUI
import TrevorbiltKit

/// The "On your table" card: the kit's ModeCard layout (TrevorbiltKit's ModeCards.swift, which this
/// port doesn't edit: the kit is changed in the SHAR port only) with its own drawing, a model of
/// the world in a box on a table. Its picture follows the kit's: the same sky and hills, the same
/// wall and floor.
struct TableModeCard: View {
    let name: String
    let line: String
    let selected: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            VStack(spacing: 6) {
                TableIllustration()
                    .frame(height: 52)
                    .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
                Text(name)
                    .font(.tbHeader(15, bold: true, relativeTo: .headline))
                    .padding(.top, 2)
                Text(line)
                    .font(.tbBody(12, weight: .medium, relativeTo: .caption))
                    .foregroundStyle(.white.opacity(0.85))
                    .multilineTextAlignment(.center)
                    .lineLimit(3)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(10)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        }
        .buttonStyle(TrevorbiltTileButtonStyle(selected: selected))
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(selected ? .isSelected : [])
    }
}

/// A wall and a floor, a table top, and on it a box holding the little world.
private struct TableIllustration: View {
    var body: some View {
        Canvas { context, size in
            let bounds = CGRect(origin: .zero, size: size)
            // The room (as the kit draws it).
            context.fill(Path(bounds), with: .color(.white.opacity(0.1)))
            var floor = Path()
            floor.move(to: CGPoint(x: bounds.minX, y: bounds.maxY))
            floor.addLine(to: CGPoint(x: bounds.minX + bounds.width * 0.12, y: bounds.minY + bounds.height * 0.8))
            floor.addLine(to: CGPoint(x: bounds.maxX - bounds.width * 0.12, y: bounds.minY + bounds.height * 0.8))
            floor.addLine(to: CGPoint(x: bounds.maxX, y: bounds.maxY))
            floor.closeSubpath()
            context.fill(floor, with: .color(.white.opacity(0.12)))
            // A table top, seen from above its near edge.
            var top = Path()
            top.move(to: CGPoint(x: size.width * 0.08, y: size.height * 0.98))
            top.addLine(to: CGPoint(x: size.width * 0.22, y: size.height * 0.66))
            top.addLine(to: CGPoint(x: size.width * 0.78, y: size.height * 0.66))
            top.addLine(to: CGPoint(x: size.width * 0.92, y: size.height * 0.98))
            top.closeSubpath()
            context.fill(top, with: .color(.white.opacity(0.24)))
            // The volume standing on it: its front face holds the world; its top and side are drawn.
            let front = CGRect(x: size.width * 0.31, y: size.height * 0.3, width: size.width * 0.32, height: size.height * 0.52)
            let depth = CGSize(width: size.width * 0.07, height: -size.height * 0.16)
            var inside = context
            inside.clip(to: Path(front))
            Self.world(in: front.insetBy(dx: -6, dy: -2), context: inside)
            var box = Path(front)
            box.move(to: front.origin)
            box.addLine(to: CGPoint(x: front.minX + depth.width, y: front.minY + depth.height))
            box.addLine(to: CGPoint(x: front.maxX + depth.width, y: front.minY + depth.height))
            box.addLine(to: CGPoint(x: front.maxX, y: front.minY))
            box.move(to: CGPoint(x: front.maxX + depth.width, y: front.minY + depth.height))
            box.addLine(to: CGPoint(x: front.maxX + depth.width, y: front.maxY + depth.height))
            box.addLine(to: CGPoint(x: front.maxX, y: front.maxY))
            context.stroke(box, with: .color(.white.opacity(0.85)), style: StrokeStyle(lineWidth: 1.5, lineJoin: .round))
        }
        .accessibilityHidden(true)
    }

    /// Sky and hills, filling `rect`.
    private static func world(in rect: CGRect, context: GraphicsContext) {
        let sky = Gradient(colors: [Trevorbilt.slate, Color(red: 0.62, green: 0.74, blue: 0.82)])
        context.fill(Path(rect), with: .linearGradient(sky, startPoint: CGPoint(x: rect.midX, y: rect.minY),
                                                       endPoint: CGPoint(x: rect.midX, y: rect.maxY)))
        func hill(_ base: CGFloat, _ peaks: [(CGFloat, CGFloat)], _ color: Color) {
            var path = Path()
            path.move(to: CGPoint(x: rect.minX, y: rect.maxY))
            path.addLine(to: CGPoint(x: rect.minX, y: rect.minY + rect.height * base))
            var x = rect.minX
            for (dx, height) in peaks {
                let next = x + rect.width * dx
                path.addQuadCurve(to: CGPoint(x: next, y: rect.minY + rect.height * base),
                                  control: CGPoint(x: (x + next) / 2, y: rect.minY + rect.height * (base - height)))
                x = next
            }
            path.addLine(to: CGPoint(x: rect.maxX, y: rect.maxY))
            path.closeSubpath()
            context.fill(path, with: .color(color))
        }
        hill(0.68, [(0.3, 0.22), (0.35, 0.3), (0.35, 0.18)], Trevorbilt.sage)
        hill(0.82, [(0.45, 0.2), (0.3, 0.12), (0.25, 0.16)], Trevorbilt.green)
    }
}
