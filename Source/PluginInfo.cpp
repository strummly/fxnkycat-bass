#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
 #include <cwchar>
 #pragma comment (lib, "version.lib")
#endif

#include "PluginInfo.h"

namespace
{
    juce::String fromModuleInfo (const juce::File& bundle)
    {
        const auto f = bundle.getChildFile ("Contents").getChildFile ("Resources").getChildFile ("moduleinfo.json");
        if (! f.existsAsFile()) return {};
        const auto text = f.loadFileAsString();

        const auto v = juce::JSON::parse (text);
        if (auto* o = v.getDynamicObject())
            if (auto* fi = o->getProperty ("Factory Info").getDynamicObject())
            {
                const auto vendor = fi->getProperty ("Vendor").toString().trim();
                if (vendor.isNotEmpty()) return vendor;
            }

        // moduleinfo.json is sometimes JSON5 (comments / trailing commas): find "Vendor" by hand.
        const int i = text.indexOf ("\"Vendor\"");
        if (i >= 0)
            return text.substring (i + 8).fromFirstOccurrenceOf ("\"", false, false)
                       .upToFirstOccurrenceOf ("\"", false, false).trim();
        return {};
    }

   #if defined (_WIN32)
    juce::String companyName (const juce::File& dll)
    {
        const auto path = dll.getFullPathName();
        DWORD handle = 0;
        const DWORD size = GetFileVersionInfoSizeW (path.toWideCharPointer(), &handle);
        if (size == 0) return {};

        juce::HeapBlock<char> data (size);
        if (! GetFileVersionInfoW (path.toWideCharPointer(), 0, size, data.get())) return {};

        auto query = [&] (const wchar_t* key) -> juce::String
        {
            wchar_t* val = nullptr; UINT len = 0;
            if (VerQueryValueW (data.get(), key, (LPVOID*) &val, &len) && val != nullptr && len > 1)
                return juce::String (val).trim();
            return {};
        };

        struct LangCodePage { WORD lang, codePage; };
        LangCodePage* trans = nullptr; UINT transLen = 0;
        if (VerQueryValueW (data.get(), L"\\VarFileInfo\\Translation", (LPVOID*) &trans, &transLen)
             && trans != nullptr && transLen >= sizeof (LangCodePage))
        {
            wchar_t key[96];
            std::swprintf (key, 96, L"\\StringFileInfo\\%04x%04x\\CompanyName", trans[0].lang, trans[0].codePage);
            const auto r = query (key);
            if (r.isNotEmpty()) return r;
        }
        for (auto* key : { L"\\StringFileInfo\\040904b0\\CompanyName", L"\\StringFileInfo\\040904e4\\CompanyName" })
        {
            const auto r = query (key);
            if (r.isNotEmpty()) return r;
        }
        return {};
    }
   #endif

   #if defined (__APPLE__)
    juce::String fromBundleIdentifier (const juce::File& bundle)
    {
        if (auto xml = juce::XmlDocument::parse (bundle.getChildFile ("Contents").getChildFile ("Info.plist")))
            if (auto* dict = xml->getChildByName ("dict"))
                for (auto* e = dict->getFirstChildElement(); e != nullptr; e = e->getNextElement())
                    if (e->hasTagName ("key") && e->getAllSubText() == "CFBundleIdentifier")
                        if (auto* val = e->getNextElement())
                        {
                            // com.vendor.product -> Vendor
                            const auto parts = juce::StringArray::fromTokens (val->getAllSubText(), ".", {});
                            if (parts.size() >= 2)
                                return parts[1].substring (0, 1).toUpperCase() + parts[1].substring (1);
                        }
        return {};
    }
   #endif
}

juce::String vc::vst3Maker (const juce::File& vst3)
{
    juce::String maker;
    if (vst3.isDirectory())
    {
        maker = fromModuleInfo (vst3);
       #if defined (_WIN32)
        if (maker.isEmpty())
        {
            // The DLL inside the bundle: usually named like the bundle, otherwise the first .vst3 there.
            const auto dir = vst3.getChildFile ("Contents").getChildFile ("x86_64-win");
            auto dll = dir.getChildFile (vst3.getFileName());
            if (! dll.existsAsFile())
                for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*.vst3", juce::File::findFiles))
                { dll = entry.getFile(); break; }
            if (dll.existsAsFile()) maker = companyName (dll);
        }
       #elif defined (__APPLE__)
        if (maker.isEmpty()) maker = fromBundleIdentifier (vst3);
       #endif
    }
   #if defined (_WIN32)
    else if (vst3.existsAsFile())
        maker = companyName (vst3);   // older single-file VST3
   #endif
    return maker.trim();
}
