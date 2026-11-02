#pragma once

#include <juce_core/juce_core.h>

namespace vc
{
    // The maker of a VST3, read from the plugin's own files without loading it
    // (so it is fast and can't crash the host):
    //   1. Contents/Resources/moduleinfo.json  ("Factory Info" -> "Vendor"), newer plugins
    //   2. Windows: the company name in the plugin DLL's file details
    //      Mac: the vendor part of the bundle identifier (com.vendor.product)
    // Empty if none of these say.
    juce::String vst3Maker (const juce::File& vst3);
}
