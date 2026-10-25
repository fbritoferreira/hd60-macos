// CoreMediaIO camera extension exposing the HD60 as "HD60 Camera" (1080p59.94, NV12).
// The card is only booted while some app is streaming from the camera.

import CoreMediaIO
import Foundation

private let frameDuration = CMTime(value: 1001, timescale: 60000)
private let width: Int32 = 1920, height: Int32 = 1080

final class ProviderSource: NSObject, CMIOExtensionProviderSource {
	private(set) var provider: CMIOExtensionProvider!
	private var deviceSource: DeviceSource!

	init(clientQueue: DispatchQueue?) {
		super.init()
		provider = CMIOExtensionProvider(source: self, clientQueue: clientQueue)
		deviceSource = DeviceSource()
		do {
			try provider.addDevice(deviceSource.device)
		} catch {
			fatalError("addDevice: \(error)")
		}
	}

	func connect(to client: CMIOExtensionClient) throws {}
	func disconnect(from client: CMIOExtensionClient) {}

	var availableProperties: Set<CMIOExtensionProperty> { [.providerManufacturer] }

	func providerProperties(forProperties properties: Set<CMIOExtensionProperty>) throws -> CMIOExtensionProviderProperties {
		let p = CMIOExtensionProviderProperties(dictionary: [:])
		if properties.contains(.providerManufacturer) { p.manufacturer = "hd60-macos" }
		return p
	}

	func setProviderProperties(_ providerProperties: CMIOExtensionProviderProperties) throws {}
}

final class DeviceSource: NSObject, CMIOExtensionDeviceSource {
	private(set) var device: CMIOExtensionDevice!
	private var streamSource: StreamSource!
	private let capture = Capture()

	override init() {
		super.init()
		device = CMIOExtensionDevice(localizedName: "HD60 Camera",
		                             deviceID: UUID(uuidString: "6D3C0F5E-60A1-4F6B-9C0E-0FD9005C0001")!,
		                             legacyDeviceID: nil, source: self)
		var desc: CMFormatDescription?
		CMVideoFormatDescriptionCreate(allocator: kCFAllocatorDefault,
		                               codecType: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
		                               width: width, height: height, extensions: nil, formatDescriptionOut: &desc)
		let format = CMIOExtensionStreamFormat(formatDescription: desc!, maxFrameDuration: frameDuration,
		                                       minFrameDuration: frameDuration, validFrameDurations: nil)
		streamSource = StreamSource(format: format, device: device)
		streamSource.onStart = { [weak self] in self?.startCapture() }
		streamSource.onStop = { [weak self] in self?.capture.stop() }
		do {
			try device.addStream(streamSource.stream)
		} catch {
			fatalError("addStream: \(error)")
		}
	}

	private func startCapture() {
		let stream = streamSource.stream!
		capture.start { pixelBuffer in
			var desc: CMFormatDescription?
			CMVideoFormatDescriptionCreateForImageBuffer(allocator: kCFAllocatorDefault, imageBuffer: pixelBuffer,
			                                             formatDescriptionOut: &desc)
			guard let desc else { return }
			let now = CMClockGetTime(CMClockGetHostTimeClock())
			var timing = CMSampleTimingInfo(duration: frameDuration, presentationTimeStamp: now, decodeTimeStamp: .invalid)
			var sample: CMSampleBuffer?
			CMSampleBufferCreateReadyWithImageBuffer(allocator: kCFAllocatorDefault, imageBuffer: pixelBuffer,
			                                         formatDescription: desc, sampleTiming: &timing,
			                                         sampleBufferOut: &sample)
			guard let sample else { return }
			stream.send(sample, discontinuity: [], hostTimeInNanoseconds: UInt64(now.seconds * Double(NSEC_PER_SEC)))
		}
	}

	var availableProperties: Set<CMIOExtensionProperty> { [.deviceModel] }

	func deviceProperties(forProperties properties: Set<CMIOExtensionProperty>) throws -> CMIOExtensionDeviceProperties {
		let p = CMIOExtensionDeviceProperties(dictionary: [:])
		if properties.contains(.deviceModel) { p.model = "Elgato Game Capture HD60" }
		return p
	}

	func setDeviceProperties(_ deviceProperties: CMIOExtensionDeviceProperties) throws {}
}

final class StreamSource: NSObject, CMIOExtensionStreamSource {
	private(set) var stream: CMIOExtensionStream!
	private let format: CMIOExtensionStreamFormat
	var onStart: (() -> Void)?
	var onStop: (() -> Void)?

	init(format: CMIOExtensionStreamFormat, device: CMIOExtensionDevice) {
		self.format = format
		super.init()
		stream = CMIOExtensionStream(localizedName: "HD60 Camera.Video",
		                             streamID: UUID(uuidString: "6D3C0F5E-60A1-4F6B-9C0E-0FD9005C0002")!,
		                             direction: .source, clockType: .hostTime, source: self)
	}

	var formats: [CMIOExtensionStreamFormat] { [format] }

	var availableProperties: Set<CMIOExtensionProperty> { [.streamActiveFormatIndex, .streamFrameDuration] }

	func streamProperties(forProperties properties: Set<CMIOExtensionProperty>) throws -> CMIOExtensionStreamProperties {
		let p = CMIOExtensionStreamProperties(dictionary: [:])
		if properties.contains(.streamActiveFormatIndex) { p.activeFormatIndex = 0 }
		if properties.contains(.streamFrameDuration) { p.frameDuration = frameDuration }
		return p
	}

	func setStreamProperties(_ streamProperties: CMIOExtensionStreamProperties) throws {}

	func authorizedToStartStream(for client: CMIOExtensionClient) -> Bool { true }

	func startStream() throws { onStart?() }

	func stopStream() throws { onStop?() }
}
