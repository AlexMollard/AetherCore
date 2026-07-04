#pragma once

// Font Awesome 6 Free-Solid codepoints used by the debug UI, as UTF-8 string
// literals. The glyphs are merged into the ImGui font atlas by ImguiSubsystem
// (resources/fonts/fa-solid-900.ttf, PUA range 0xE000-0xF8FF) so these can be
// embedded directly in any ImGui text, e.g.:
//
//     ImGui::Button(ICON_FA_PLUS "  Create");
//
// Deliberately a hand-picked subset - add codepoints as the tooling needs them
// (https://fontawesome.com/icons, "free" + "solid" filters).

// Entity kinds
#define ICON_FA_CUBE "\xef\x86\xb2"                    // U+F1B2 mesh
#define ICON_FA_PERSON_RUNNING "\xef\x9c\x8c"          // U+F70C skinned mesh
#define ICON_FA_WEIGHT_HANGING "\xef\x97\x8d"          // U+F5CD physics body
#define ICON_FA_WAND_MAGIC_SPARKLES "\xee\x8b\x8a"     // U+E2CA effect-driven
#define ICON_FA_CIRCLE "\xef\x84\x91"                  // U+F111 plain entity

// Toolbar / actions
#define ICON_FA_MAGNIFYING_GLASS "\xef\x80\x82"        // U+F002 search
#define ICON_FA_PLUS "\xef\x81\xa7"                    // U+F067 create
#define ICON_FA_TRASH "\xef\x87\xb8"                   // U+F1F8 delete
#define ICON_FA_PEN "\xef\x8c\x84"                     // U+F304 rename
#define ICON_FA_XMARK "\xef\x80\x8d"                   // U+F00D close/remove
#define ICON_FA_FILTER "\xef\x82\xb0"                  // U+F0B0 type filters

// Inspector sections
#define ICON_FA_UP_DOWN_LEFT_RIGHT "\xef\x82\xb2"      // U+F0B2 transform
#define ICON_FA_PALETTE "\xef\x94\xbf"                 // U+F53F material
#define ICON_FA_FILM "\xef\x80\x88"                    // U+F008 animation
#define ICON_FA_SITEMAP "\xef\x83\xa8"                 // U+F0E8 hierarchy
#define ICON_FA_TAG "\xef\x80\xab"                     // U+F02B tags
#define ICON_FA_BOLT "\xef\x83\xa7"                    // U+F0E7 effect params
#define ICON_FA_GEARS "\xef\x82\x85"                   // U+F085 pipeline/render
#define ICON_FA_IMAGE "\xef\x80\xbe"                   // U+F03E texture
#define ICON_FA_LINK "\xef\x83\x81"                    // U+F0C1 parent link
#define ICON_FA_EYE "\xef\x81\xae"                     // U+F06E visibility
#define ICON_FA_ROTATE "\xef\x8b\xb1"                  // U+F2F1 reset/refresh
#define ICON_FA_DIAGRAM_PROJECT "\xef\x95\x82"         // U+F542 render graph
