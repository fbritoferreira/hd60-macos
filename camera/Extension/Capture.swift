// Runs the HD60 driver core (src/device.c) on a background thread and turns its MPEG-TS into
// decoded frames: TS demux -> H.264 access units -> VideoToolbox -> display-order pixel buffers.

import Foundation
import os
import VideoToolbox

private let log = Logger(subsystem: "com.fbritoferreira.hd60camera", category: "capture")

final class Capture {
	private var thread: Thread?
	private let stopFlag = UnsafeMutablePointer<Int32>.allocate(capacity: 1)
	private let finished = DispatchSemaphore(value: 0)
	private let demux = TSDemux()
	private let decoder = H264Decoder()

	func start(onFrame: @escaping (CVPixelBuffer) -> Void) {
		guard thread == nil else { return }
		guard let firmware = Bundle.main.path(forResource: "hd60-bridge", ofType: "bin") else {
			log.error("hd60-bridge.bin missing from the extension bundle")
			return
		}
		stopFlag.pointee = 0
		decoder.onFrame = onFrame
		demux.onAccessUnit = { [decoder] data, pts in decoder.decode(data, pts: pts) }
		let t = Thread { [self] in run(firmware: firmware) }
		t.name = "hd60-capture"
		t.qualityOfService = .userInteractive
		thread = t
		t.start()
	}

	func stop() {
		guard thread != nil else { return }
		stopFlag.pointee = 1
		finished.wait() // the device core shuts the card down before returning
		thread = nil
		decoder.reset()
		demux.reset()
	}

	private func run(firmware: String) {
		defer { finished.signal() }
		let ctx = Unmanaged.passUnretained(demux).toOpaque()
		firmware.withCString { fw in
			var cfg = hd60_config(firmware: fw, mode: nil, bitrate_kbps: 0, verbose: 0)
			var last: Int32 = 1
			while stopFlag.pointee == 0 {
				let r = hd60_run(&cfg, { buf, len, ctx in
					guard let buf, let ctx else { return }
					Unmanaged<TSDemux>.fromOpaque(ctx).takeUnretainedValue().feed(buf, Int(len))
				}, ctx, stopFlag)
				if stopFlag.pointee != 0 { break }
				if Int(r) == Int(HD60_ERR_FIRMWARE) {
					log.error("firmware rejected")
					break
				}
				if r != last {
					log.info("hd60_run returned \(r); retrying")
					last = r
				}
				sleep(1)
			}
		}
	}
}

/// Minimal MPEG-TS demuxer: finds the H.264 stream through PAT/PMT and emits whole PES
/// payloads (one access unit each on this encoder) with their 90 kHz PTS.
final class TSDemux {
	var onAccessUnit: ((Data, Int64?) -> Void)?
	private var carry = [UInt8]()
	private var pmtPID = -1
	private var videoPID = -1
	private var pes = [UInt8]()

	func reset() {
		carry.removeAll()
		pes.removeAll()
		pmtPID = -1
		videoPID = -1
	}

	func feed(_ p: UnsafePointer<UInt8>, _ n: Int) {
		carry.append(contentsOf: UnsafeBufferPointer(start: p, count: n))
		var i = 0
		while i + 188 <= carry.count {
			// Resync: a packet starts with 0x47 and so does the next one.
			if carry[i] != 0x47 || (i + 376 <= carry.count && carry[i + 188] != 0x47) {
				i += 1
				continue
			}
			packet(carry[i..<i + 188])
			i += 188
		}
		carry.removeFirst(i)
	}

	private func packet(_ pkt: ArraySlice<UInt8>) {
		let b = Array(pkt)
		let pusi = b[1] & 0x40 != 0
		let pid = Int(b[1] & 0x1f) << 8 | Int(b[2])
		let afc = (b[3] >> 4) & 3
		guard afc & 1 != 0 else { return }
		var off = 4
		if afc & 2 != 0 { off += 1 + Int(b[4]) }
		guard off < 188 else { return }
		let payload = b[off...]

		if pid == 0, pusi {
			parsePAT(Array(payload))
		} else if pid == pmtPID, pusi {
			parsePMT(Array(payload))
		} else if pid == videoPID {
			if pusi {
				flush()
			}
			if pusi || !pes.isEmpty { pes.append(contentsOf: payload) }
		}
	}

	private func section(_ p: [UInt8]) -> ArraySlice<UInt8>? {
		guard !p.isEmpty else { return nil }
		let start = 1 + Int(p[0])
		guard start + 3 <= p.count else { return nil }
		let len = Int(p[start + 1] & 0x0f) << 8 | Int(p[start + 2])
		let end = min(p.count, start + 3 + len)
		return p[start..<end]
	}

	private func parsePAT(_ p: [UInt8]) {
		guard let s = section(p).map(Array.init), s.count >= 12 else { return }
		var i = 8
		while i + 4 <= s.count - 4 {
			let program = Int(s[i]) << 8 | Int(s[i + 1])
			let pid = Int(s[i + 2] & 0x1f) << 8 | Int(s[i + 3])
			if program != 0 {
				pmtPID = pid
				return
			}
			i += 4
		}
	}

	private func parsePMT(_ p: [UInt8]) {
		guard let s = section(p).map(Array.init), s.count >= 16 else { return }
		let infoLen = Int(s[10] & 0x0f) << 8 | Int(s[11])
		var i = 12 + infoLen
		while i + 5 <= s.count - 4 {
			let type = s[i]
			let pid = Int(s[i + 1] & 0x1f) << 8 | Int(s[i + 2])
			let esLen = Int(s[i + 3] & 0x0f) << 8 | Int(s[i + 4])
			if type == 0x1b {
				videoPID = pid
				return
			}
			i += 5 + esLen
		}
	}

	private func flush() {
		defer { pes.removeAll(keepingCapacity: true) }
		guard pes.count > 9, pes[0] == 0, pes[1] == 0, pes[2] == 1 else { return }
		let headerLen = Int(pes[8])
		var pts: Int64?
		if pes[7] & 0x80 != 0, pes.count >= 14 {
			let b = pes
			pts = Int64(b[9] & 0x0e) << 29 | Int64(b[10]) << 22 | Int64(b[11] & 0xfe) << 14
				| Int64(b[12]) << 7 | Int64(b[13]) >> 1
		}
		let start = 9 + headerLen
		guard start < pes.count else { return }
		onAccessUnit?(Data(pes[start...]), pts)
	}
}

/// Hardware H.264 decode with VideoToolbox, plus a small PTS reorder buffer because the
/// encoder uses B-frames (decode order != display order).
final class H264Decoder {
	var onFrame: ((CVPixelBuffer) -> Void)?
	private var session: VTDecompressionSession?
	private var format: CMVideoFormatDescription?
	private var sps: Data?
	private var pps: Data?
	private var reorder: [(pts: Int64, buffer: CVPixelBuffer)] = []
	private let reorderDepth = 2 // the stream's two B-frames

	func reset() {
		if let session { VTDecompressionSessionInvalidate(session) }
		session = nil
		format = nil
		sps = nil
		pps = nil
		reorder.removeAll()
	}

	func decode(_ au: Data, pts: Int64?) {
		var slices = [Data]()
		for nal in splitAnnexB(au) {
			switch nal[nal.startIndex] & 0x1f {
			case 7:
				if nal != sps { sps = nal; format = nil }
			case 8:
				if nal != pps { pps = nal; format = nil }
			case 9:
				continue // access unit delimiter
			default:
				slices.append(nal)
			}
		}
		if format == nil, !makeSession() { return }
		guard let session, let format, !slices.isEmpty else { return }

		var avcc = Data()
		for s in slices {
			var len = UInt32(s.count).bigEndian
			avcc.append(Data(bytes: &len, count: 4))
			avcc.append(s)
		}
		var block: CMBlockBuffer?
		let count = avcc.count
		guard CMBlockBufferCreateWithMemoryBlock(allocator: kCFAllocatorDefault, memoryBlock: nil, blockLength: count,
		                                         blockAllocator: kCFAllocatorDefault, customBlockSource: nil,
		                                         offsetToData: 0, dataLength: count, flags: 0,
		                                         blockBufferOut: &block) == noErr, let block else { return }
		avcc.withUnsafeBytes { _ = CMBlockBufferReplaceDataBytes(with: $0.baseAddress!, blockBuffer: block,
		                                                          offsetIntoDestination: 0, dataLength: count) }
		var timing = CMSampleTimingInfo(duration: .invalid,
		                                presentationTimeStamp: CMTime(value: pts ?? 0, timescale: 90000),
		                                decodeTimeStamp: .invalid)
		var sample: CMSampleBuffer?
		var size = count
		guard CMSampleBufferCreateReady(allocator: kCFAllocatorDefault, dataBuffer: block, formatDescription: format,
		                                sampleCount: 1, sampleTimingEntryCount: 1, sampleTimingArray: &timing,
		                                sampleSizeEntryCount: 1, sampleSizeArray: &size,
		                                sampleBufferOut: &sample) == noErr, let sample else { return }

		VTDecompressionSessionDecodeFrame(session, sampleBuffer: sample, flags: [], infoFlagsOut: nil) {
			[weak self] status, _, image, pts, _ in
			guard status == noErr, let image, let self else { return }
			self.reorder.append((pts.value, image))
			self.reorder.sort { $0.pts < $1.pts }
			if self.reorder.count > self.reorderDepth {
				self.onFrame?(self.reorder.removeFirst().buffer)
			}
		}
	}

	private func makeSession() -> Bool {
		guard let sps, let pps else { return false }
		if let session { VTDecompressionSessionInvalidate(session) }
		session = nil
		var desc: CMFormatDescription?
		let status = sps.withUnsafeBytes { s in
			pps.withUnsafeBytes { p in
				let ptrs = [s.bindMemory(to: UInt8.self).baseAddress!, p.bindMemory(to: UInt8.self).baseAddress!]
				let sizes = [sps.count, pps.count]
				return CMVideoFormatDescriptionCreateFromH264ParameterSets(
					allocator: kCFAllocatorDefault, parameterSetCount: 2, parameterSetPointers: ptrs,
					parameterSetSizes: sizes, nalUnitHeaderLength: 4, formatDescriptionOut: &desc)
			}
		}
		guard status == noErr, let desc else { return false }
		let attrs: [CFString: Any] = [
			kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
			kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary,
		]
		var s: VTDecompressionSession?
		guard VTDecompressionSessionCreate(allocator: kCFAllocatorDefault, formatDescription: desc,
		                                   decoderSpecification: nil, imageBufferAttributes: attrs as CFDictionary,
		                                   outputCallback: nil, decompressionSessionOut: &s) == noErr else { return false }
		session = s
		format = desc
		reorder.removeAll()
		return true
	}

	/// Splits an Annex B byte stream (00 00 01 / 00 00 00 01 start codes) into NAL units.
	private func splitAnnexB(_ d: Data) -> [Data] {
		var out = [Data]()
		let b = [UInt8](d)
		var i = 0, start = -1
		while i + 2 < b.count {
			if b[i] == 0, b[i + 1] == 0, b[i + 2] == 1 {
				if start >= 0 {
					var end = i
					while end > start, b[end - 1] == 0 { end -= 1 }
					if end > start { out.append(Data(b[start..<end])) }
				}
				i += 3
				start = i
			} else {
				i += 1
			}
		}
		if start >= 0, start < b.count { out.append(Data(b[start...])) }
		return out
	}
}
