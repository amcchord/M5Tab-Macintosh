#!/usr/bin/env swift

import CoreGraphics
import Foundation
import ImageIO
import Vision

guard CommandLine.arguments.count == 2 else {
    FileHandle.standardError.write(Data("usage: ocr_png.swift IMAGE.png\n".utf8))
    exit(2)
}

let imageURL = URL(fileURLWithPath: CommandLine.arguments[1])
guard let source = CGImageSourceCreateWithURL(imageURL as CFURL, nil),
      let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else {
    FileHandle.standardError.write(Data("could not decode \(imageURL.path)\n".utf8))
    exit(1)
}

let request = VNRecognizeTextRequest()
request.recognitionLevel = .accurate
request.usesLanguageCorrection = false
request.minimumTextHeight = 0.012

let handler = VNImageRequestHandler(cgImage: image, options: [:])
do {
    try handler.perform([request])
} catch {
    FileHandle.standardError.write(Data("OCR failed: \(error)\n".utf8))
    exit(1)
}

let observations = (request.results ?? []).sorted {
    let rowDelta = $0.boundingBox.midY - $1.boundingBox.midY
    if abs(rowDelta) > 0.015 { return rowDelta > 0 }
    return $0.boundingBox.minX < $1.boundingBox.minX
}

for observation in observations {
    guard let candidate = observation.topCandidates(1).first else { continue }
    let box = observation.boundingBox
    let record: [String: Any] = [
        "text": candidate.string,
        "confidence": candidate.confidence,
        "x": box.minX,
        "y": box.minY,
        "width": box.width,
        "height": box.height,
    ]
    let data = try JSONSerialization.data(withJSONObject: record, options: [.sortedKeys])
    print(String(decoding: data, as: UTF8.self))
}
