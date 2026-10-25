// Sreon — main.swift
import Cocoa
import CoreServices

let sreonApp = NSApplication.shared
sreonApp.delegate = sreonDelegate
sreonApp.setActivationPolicy(.regular)
sreonApp.run()
