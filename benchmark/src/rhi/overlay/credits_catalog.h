/*
 * Shared text catalog for the Credits UI and docs/CREDITS.md.
 * Original names/roles transcribe Heaven's credits_16x9/4x3.jpg, not guesses.
 * These external reference images are not bundled with HeavenBench.
 */
#ifndef HEAVENBENCH_SRC_RHI_OVERLAY_CREDITS_CATALOG_H_
#define HEAVENBENCH_SRC_RHI_OVERLAY_CREDITS_CATALOG_H_

#include <array>
#include <span>

namespace heaven::credits {

struct Entry {
    const char* name;
    const char* role;
};

struct Section {
    const char* title;
    std::span<const Entry> entries;
};

inline constexpr std::array kHeavenBench{
    Entry{"Gabriel Max <dev@zetta.app>", "HeavenBench development and modernization"},
};
inline constexpr std::array kExtraction{
    Entry{"Luigi Auriemma", "Original UNG extraction tool (2012-2019)"},
};

inline constexpr std::array kContent{
    Entry{"Pluton (Andrey Kushner)", "3D artist"},
    Entry{"Andrewko (Andrey Kolchin)", "3D artist"},
    Entry{"Brrrr (Stanislav Zagniy)", "3D artist"},
    Entry{"Black_D_N (Dmitriy Cherniy)", "GUI designer"},
    Entry{"Jane (Eugenia Shergina)", "GUI designer"},
    Entry{"Paralyzah (Mike Kondratyev)", "Musician"},
    Entry{"Manguste (Nadezhda Ovchinnikova)", "Localization"},
};
inline constexpr std::array kTechnology{
    Entry{"Frustum (Alexander Zapryagaev)", "Engine & logic programmer"},
    Entry{"Johnatan (Evgeniy Tetenchuk)", "Launcher programmer"},
    Entry{"Anna (Anna Chaplinskaya)", "Build engineer"},
};
inline constexpr std::array kQualityAssurance{
    Entry{"Necris (Dmitry Kriventsov)", "QA lead"},
    Entry{"Silent (Andrew Bayun)", "QA engineer"},
    Entry{"Sebastianbah (Kate Popova)", "QA engineer"},
};
inline constexpr std::array kProduction{
    Entry{"Binstream (Denis Shergin)", "Producer"},
    Entry{"Natalia (Natalia Ryukhova)", "PM assistant"},
};
inline constexpr std::array kThanks{
    Entry{"TmifX (Ivan Gulyaev)", "Logic programmer"},
    Entry{"FigoNEt (Pavel Kurinnoy)", "Logic programmer"},
    Entry{"Jashik (Yakov Negrey)", "GUI programmer"},
    Entry{"MrRed (Sergey Kanashchuk)", "QA engineer"},
    Entry{"Watchvombat (Polina Ignatova)", "QA engineer"},
    Entry{"Ilmarinen (Serge Larin)", "PM assistant"},
};

inline constexpr std::array kThirdParty{
    Entry{"Google Filament", "Google and contributors; rendering and bundled sky textures (Apache-2.0)"},
    Entry{"SDL2", "Sam Lantinga and contributors; platform integration (zlib license)"},
    Entry{"Dear ImGui", "Omar Cornut; user interface (MIT)"},
    Entry{"Jolt Physics", "Jorrit Rouwe; physics (MIT)"},
    Entry{"GLM", "G-Truc Creation / Christophe Riccio; mathematics (MIT)"},
    Entry{"OpenSubdiv", "Pixar, DreamWorks Animation, Autodesk, Google and DigitalFish; subdivision (modified Apache-2.0)"},
    Entry{"libzip", "Dieter Baron and Thomas Klausner; asset archives (BSD-3-Clause)"},
    Entry{"zlib", "Jean-loup Gailly and Mark Adler; compression (zlib license)"},
    Entry{"LZ4", "Yann Collet; compression (BSD-2-Clause)"},
    Entry{"bcdec", "Sergii \"iOrange\" Kudlai; texture decoding (MIT / Unlicense)"},
    Entry{"pocketfft", "Martin Reinecke, Peter Bell and Max-Planck-Society; Fourier ground-height reconstruction at startup fallback and in the packer (BSD-3-Clause)"},
    Entry{"DeepBump", "Hugo Tini; adapted normal-to-height method (GPL-3.0)"},
    Entry{"stb / stb_vorbis", "Sean Barrett and contributors; image/audio decoding (MIT / public-domain option)"},
    Entry{"MikkTSpace", "Morten S. Mikkelsen; tangent generation (zlib license)"},
    Entry{"nlohmann/json", "Niels Lohmann; JSON data (MIT)"},
    Entry{"RapidCheck", "Emil Eriksson; test infrastructure (BSD-2-Clause)"},
    Entry{"EnvironmentalVisualEnhancements / BoulderCo", "Ryan Bray, Warwick Allison and Ghassen Lahmar; retained cloud reference textures (MIT)"},
};

inline constexpr std::array kSections{
    Section{"HeavenBench", kHeavenBench},
    Section{"Original UNG extraction tool", kExtraction},
    Section{"Original Heaven - Content", kContent},
    Section{"Original Heaven - Technology", kTechnology},
    Section{"Original Heaven - Quality Assurance", kQualityAssurance},
    Section{"Original Heaven - Production", kProduction},
    Section{"Original Heaven - Thanks to", kThanks},
    Section{"Third-party acknowledgments", kThirdParty},
};

inline constexpr char kOriginalNotice[] =
    "Unigine Corp. \xC2\xA9 2005-2013. All rights reserved.";
inline constexpr char kAssetNotice[] =
    "Original Heaven content and music are credited to their original authors. "
    "Heaven assets are supplied separately and remain subject to their original terms.";
inline constexpr char kLicenseNotice[] =
    "This acknowledgment summary does not replace license texts. "
    "See docs/CREDITS.md and the licenses and notices accompanying release packages "
    "for dependency and transitive-component details.";

} // namespace heaven::credits

#endif // HEAVENBENCH_SRC_RHI_OVERLAY_CREDITS_CATALOG_H_
