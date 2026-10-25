// Host app for the HD60 camera extension. macOS only activates system extensions on request
// from an app in /Applications, so this is a small window with Install / Uninstall buttons.

import SwiftUI
import SystemExtensions

private let extensionID = "com.fbritoferreira.hd60camera.extension"

@main
struct HD60CameraApp: App {
	@StateObject private var installer = Installer()

	var body: some Scene {
		WindowGroup("HD60 Camera") {
			VStack(alignment: .leading, spacing: 12) {
				Text("Elgato Game Capture HD60").font(.headline)
				Text("Install the camera extension, then pick “HD60 Camera” as a video source in OBS, FaceTime or any other app.")
					.fixedSize(horizontal: false, vertical: true)
				HStack {
					Button("Install") { installer.submit(activate: true) }
					Button("Uninstall") { installer.submit(activate: false) }
				}
				Text(installer.status).font(.callout).foregroundStyle(.secondary)
					.fixedSize(horizontal: false, vertical: true)
			}
			.padding(20)
			.frame(width: 420)
		}
		.windowResizability(.contentSize)
	}
}

final class Installer: NSObject, ObservableObject, OSSystemExtensionRequestDelegate {
	@Published var status = ""

	func submit(activate: Bool) {
		let request = activate
			? OSSystemExtensionRequest.activationRequest(forExtensionWithIdentifier: extensionID, queue: .main)
			: OSSystemExtensionRequest.deactivationRequest(forExtensionWithIdentifier: extensionID, queue: .main)
		request.delegate = self
		OSSystemExtensionManager.shared.submitRequest(request)
		status = activate ? "Installing…" : "Uninstalling…"
	}

	func request(_ request: OSSystemExtensionRequest,
	             actionForReplacingExtension existing: OSSystemExtensionProperties,
	             withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
		.replace
	}

	func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
		status = "Approve it in System Settings › General › Login Items & Extensions › Camera Extensions."
	}

	func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
		status = result == .completed ? "Done." : "Done; a restart is needed to finish."
	}

	func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
		status = "Failed: \(error.localizedDescription)"
	}
}
