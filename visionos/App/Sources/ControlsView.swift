import GameController
import SwiftUI
import TrevorbiltKit

/// The launcher's Controls tab, for each way of playing: a gamepad (DualSense-style or Xbox-style,
/// as connected) or the Sense controllers, drawn as TrevorbiltKit's own illustrations with a
/// callout on each control that does something (the connected controller's own symbol for it, or
/// the drawing's family's with nothing connected, what it does and its name); and bare hands, the
/// kit's drawing with numbered markers and a legend, each hand a random skin tone each time the
/// page appears. The mapping is the port's (Port.controls). No separate Controls window: in an
/// immersive space it would catch the game's pinches.
struct ControlsView: View {
    enum Input: String, CaseIterable, Identifiable {
        case hands = "Hands", sense = "Sense", gamepad = "Gamepad"
        var id: Self { self }
    }

    /// The immersive modes this port offers, as the chips name them ("Full", or "Full, Progressive").
    static var immersiveModesName: String {
        Port.modes.contains(.progressive) ? "Full, Progressive" : "Full"
    }

    /// The controls every drawing knows, as the kit names them.
    static let kitControls = ["leftTrigger", "leftShoulder", "leftStick", "dpad", "y", "x", "view",
                              "rightTrigger", "rightShoulder", "rightStick", "b", "a", "menu"]

    @State private var guide = ControlsGuide.shared

    var body: some View {
        ScrollView {
            VStack(spacing: 12) {
                TrevorbiltHeading("how to ", bold: "play", size: 22)
                HStack(spacing: 8) {
                    ForEach(Port.controlInputs) { input in inputButton(input) }
                    if !Port.controlPages.isEmpty {
                        Spacer().frame(width: 8)
                        ForEach(Port.controlPages.indices, id: \.self) { page in
                            TrevorbiltChip(Port.controlPages[page], selected: guide.page == page) { guide.page = page }
                        }
                    }
                }
                page
            }
            .padding(24)
            .frame(maxWidth: .infinity)
        }
    }

    private func inputButton(_ input: Input) -> some View {
        let connected = switch input {
        case .hands: false
        case .sense: guide.senseConnected
        case .gamepad: guide.gamepadConnected
        }
        return Button { guide.input = input } label: {
            VStack(spacing: 3) {
                Group {
                    switch input {
                    case .hands: Image(systemName: "hand.raised.fill")
                    case .sense: HStack(spacing: 2) { Image(systemName: "l.joystick.fill"); Image(systemName: "r.joystick.fill") }
                    case .gamepad: Image(systemName: "gamecontroller.fill")
                    }
                }
                .symbolRenderingMode(.hierarchical)
                .font(.system(size: 20, weight: .medium))
                .frame(height: 24)
                .accessibilityHidden(true)
                Text(input.rawValue).font(.tbHeader(13, bold: true, relativeTo: .headline))
                Text(connected ? "Connected" : input == .hands ? Self.immersiveModesName : "Not connected")
                    .font(.tbBody(11, weight: .medium, relativeTo: .caption2))
                    .foregroundStyle(.white.opacity(connected ? 1 : 0.8))
                    .lineLimit(1)
                    .minimumScaleFactor(0.85)
                    .padding(.horizontal, 4)
            }
            .padding(.vertical, 8)
            .frame(width: 104)
        }
        .buttonStyle(TrevorbiltTileButtonStyle(selected: guide.input == input))
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(guide.input == input ? .isSelected : [])
    }

    @ViewBuilder private var page: some View {
        let window = guide.windowView && guide.input != .hands
        let items = Port.controls(guide.input, window: window, page: guide.page)
        switch guide.input {
        case .hands:
            HStack(alignment: .center, spacing: 14) {
                ControllerDiagram(art: .hands, items: items, markerSize: 20)
                    .frame(minWidth: 0, maxWidth: .infinity)
                ControlsLegend(items: items)
                    .frame(width: 290)
            }
            // A tone for each hand from Crayola's Colors of the World, picked afresh each time the
            // page appears, never baked in (Trevor's call). A port with its own hands (SHAR's
            // yellow) draws its own art instead.
            .randomHandTones()
        case .gamepad:
            ControllerCallouts(rig: padRig, controls: called(items).map { glyph($0, items: items, on: .gamepad, rig: padRig) })
        case .sense:
            ControllerCallouts(rig: .sensePair, controls: called(items).map { glyph($0, items: items, on: .sense, rig: .sensePair) })
                // What no button does (a swing), in the room under the hands.
                .overlay(alignment: .bottom) {
                    unplaced(items).padding(.bottom, 6).dynamicTypeSize(...DynamicTypeSize.xxLarge)
                }
        }
        footnote(window: window)
    }

    /// The kit's controls these items use, in the drawing's order.
    private func called(_ items: [ControlItem]) -> [String] {
        let used = Set(items.compactMap(\.anchor) + items.flatMap(\.also.keys))
        return Self.kitControls.filter(used.contains)
    }

    @ViewBuilder private func unplaced(_ items: [ControlItem]) -> some View {
        let rest = items.filter { $0.anchor.map { !Self.kitControls.contains($0) } ?? true }
        if !rest.isEmpty {
            HStack(spacing: 14) {
                ForEach(rest) { item in
                    HStack(spacing: 6) {
                        Image(systemName: item.anchor == "swing" ? "hand.wave.fill" : "info.circle")
                            .symbolRenderingMode(.hierarchical)
                            .accessibilityHidden(true)
                        Text("\(Text(item.action).bold()): \(item.how)")
                    }
                    .font(.tbBody(12, relativeTo: .caption))
                }
            }
        }
    }

    /// The gamepad drawing: a DualSense or DualShock as the DualSense kind, any other pad as the
    /// Xbox kind, the DualSense kind with none connected.
    private var padRig: ControllerRig {
        _ = guide.controllerChanges
        // LMVR_TEST_PAD=none|xbox: the Simulator always has its own pad.
        if let test = TestHooks.value("LMVR_TEST_PAD") { return test == "xbox" ? .xbox : .dualSense }
        guard let pad = GCController.controllers().first(where: {
            $0.productCategory != GCProductCategorySpatialController && $0.extendedGamepad != nil
        })?.extendedGamepad else { return .dualSense }
        return pad is GCDualSenseGamepad || pad is GCDualShockGamepad ? .dualSense : .xbox
    }

    /// A control as drawn: the connected controller's symbol (or the family's), what it does here,
    /// and its name in that family.
    private func glyph(_ control: String, items: [ControlItem], on input: Input, rig: ControllerRig) -> GlyphControl {
        var actions = items.filter { $0.anchor == control }.map(\.action)
        actions += items.compactMap { $0.also[control] }
        let connected = TestHooks.value("LMVR_TEST_PAD") == nil ? ControllerGlyph(anchor: control)?.symbol(on: input) : nil
        return GlyphControl(control, symbol: connected ?? GlyphControl.neutralSymbol(control, for: rig), actions: actions,
                            name: Self.controlName(control, rig: rig))
    }

    private static func controlName(_ control: String, rig: ControllerRig) -> String {
        switch (rig, control) {
        case (_, "leftStick"): "Left stick"
        case (_, "rightStick"): "Right stick"
        case (_, "dpad"): "D-pad"
        case (.xbox, "a"): "A"
        case (.xbox, "b"): "B"
        case (.xbox, "x"): "X"
        case (.xbox, "y"): "Y"
        case (.xbox, "leftShoulder"): "LB"
        case (.xbox, "rightShoulder"): "RB"
        case (.xbox, "leftTrigger"): "LT"
        case (.xbox, "rightTrigger"): "RT"
        case (.xbox, "menu"): "Menu"
        case (.xbox, "view"): "View"
        case (_, "a"): "Cross"
        case (_, "b"): "Circle"
        case (_, "x"): "Square"
        case (_, "y"): "Triangle"
        case (.sensePair, "leftShoulder"): "L1 (grip)"
        case (.sensePair, "rightShoulder"): "R1 (grip)"
        case (.sensePair, "leftTrigger"): "L2 (trigger)"
        case (.sensePair, "rightTrigger"): "R2 (trigger)"
        case (_, "leftShoulder"): "L1"
        case (_, "rightShoulder"): "R1"
        case (_, "leftTrigger"): "L2"
        case (_, "rightTrigger"): "R2"
        case (_, "menu"): "Options"
        case (_, "view"): "Create"
        default: control
        }
    }

    private func footnote(window: Bool) -> some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: "info.circle.fill")
                .symbolRenderingMode(.hierarchical)
                .font(.system(size: 16))
                .foregroundStyle(.white.opacity(0.8))
                .accessibilityHidden(true)
            Text(Port.controlsNote(guide.input, window: window))
                .font(.tbBody(12, relativeTo: .caption))
                .foregroundStyle(.white.opacity(0.8))
                .fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
            // Which view's buttons these are, the other a tap away (hands play only immersive).
            if guide.input != .hands && Port.modes.contains(.window) {
                HStack(spacing: 6) {
                    TrevorbiltChip(Self.immersiveModesName, selected: !window) { guide.windowView = false }
                    TrevorbiltChip(GameModel.Immersion.window.title, selected: window) { guide.windowView = true }
                }
                .fixedSize()
                .dynamicTypeSize(...DynamicTypeSize.xxLarge)
            }
        }
        .padding(10)
        .background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 14, style: .continuous))
    }
}

/// What the guide shows (the launcher opens it on what's connected, for the selected mode), and
/// which controllers are connected.
@Observable
final class ControlsGuide {
    static let shared = ControlsGuide()
    var input = ControlsView.Input.gamepad
    var page = 0
    /// The Window view's buttons rather than Full and Progressive's.
    var windowView = false
    /// Counts controllers connecting and disconnecting; reading it redraws with the new symbols.
    private(set) var controllerChanges = 0

    var senseConnected: Bool {
        _ = controllerChanges
        return GCController.controllers().contains { $0.productCategory == GCProductCategorySpatialController }
    }

    var gamepadConnected: Bool {
        _ = controllerChanges
        return GCController.controllers().contains { $0.productCategory != GCProductCategorySpatialController && $0.extendedGamepad != nil }
    }

    private init() {
        for name in [Notification.Name.GCControllerDidConnect, .GCControllerDidDisconnect] {
            NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                self?.controllerChanges += 1
            }
        }
    }
}

/// A button as the connected controller draws it (GCControllerElement.sfSymbolsName). Sense
/// controllers name each half's elements alike, so X and Y are the left half's A and B. Never a
/// maker's artwork: symbols only.
enum ControllerGlyph {
    case a, b, x, y, menu, view, dpad, leftTrigger, rightTrigger, leftShoulder, rightShoulder, leftStick, rightStick

    init?(anchor: String) {
        switch anchor {
        case "a": self = .a
        case "b": self = .b
        case "x": self = .x
        case "y": self = .y
        case "menu": self = .menu
        case "view": self = .view
        case "dpad": self = .dpad
        case "leftTrigger": self = .leftTrigger
        case "rightTrigger": self = .rightTrigger
        case "leftShoulder": self = .leftShoulder
        case "rightShoulder": self = .rightShoulder
        case "leftStick": self = .leftStick
        case "rightStick": self = .rightStick
        default: return nil
        }
    }

    /// The symbol on the connected controller of that kind, or nil with none connected.
    func symbol(on input: ControlsView.Input) -> String? {
        _ = ControlsGuide.shared.controllerChanges
        let controllers = GCController.controllers()
        switch input {
        case .hands:
            return nil
        case .sense:
            let spatial = controllers.filter { $0.productCategory == GCProductCategorySpatialController }
            let left = spatial.first { $0.vendorName?.hasSuffix("(L)") == true }
            let right = spatial.first { $0.vendorName?.hasSuffix("(R)") == true }
            func button(_ half: GCController?, _ names: String...) -> String? {
                names.lazy.compactMap { half?.physicalInputProfile.buttons[$0]?.sfSymbolsName }.first
            }
            func stick(_ half: GCController?) -> String? {
                half?.physicalInputProfile.dpads[__GCInputDirectionPadName.thumbstick.rawValue]?.sfSymbolsName
            }
            switch self {
            case .a: return button(right, GCInputButtonA)
            case .b: return button(right, GCInputButtonB)
            case .x: return button(left, GCInputButtonA)
            case .y: return button(left, GCInputButtonB)
            case .menu: return button(right, GCInputButtonMenu)
            case .view: return button(left, GCInputButtonShare, GCInputButtonOptions, GCInputButtonMenu)
            case .leftTrigger: return button(left, __GCInputButtonName.trigger.rawValue)
            case .rightTrigger: return button(right, __GCInputButtonName.trigger.rawValue)
            case .leftShoulder: return button(left, "Grip")
            case .rightShoulder: return button(right, "Grip")
            case .leftStick: return stick(left)
            case .rightStick: return stick(right)
            case .dpad: return nil
            }
        case .gamepad:
            guard let pad = controllers.first(where: {
                $0.productCategory != GCProductCategorySpatialController && $0.extendedGamepad != nil
            })?.extendedGamepad else { return nil }
            switch self {
            case .a: return pad.buttonA.sfSymbolsName
            case .b: return pad.buttonB.sfSymbolsName
            case .x: return pad.buttonX.sfSymbolsName
            case .y: return pad.buttonY.sfSymbolsName
            case .menu: return pad.buttonMenu.sfSymbolsName
            case .view: return pad.buttonOptions?.sfSymbolsName
            case .dpad: return pad.dpad.sfSymbolsName
            case .leftTrigger: return pad.leftTrigger.sfSymbolsName
            case .rightTrigger: return pad.rightTrigger.sfSymbolsName
            case .leftShoulder: return pad.leftShoulder.sfSymbolsName
            case .rightShoulder: return pad.rightShoulder.sfSymbolsName
            case .leftStick: return pad.leftThumbstick.sfSymbolsName
            case .rightStick: return pad.rightThumbstick.sfSymbolsName
            }
        }
    }
}
