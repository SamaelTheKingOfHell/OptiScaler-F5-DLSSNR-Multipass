#pragma once

#include "menu_common.h"

// A section of a menu page as a card (ScopedCollapsingHeader: border, padding, themed header), with the page's control
// width inside it: a card is a child window, which starts with its own default item width. A card opened inside
// another card is a flat group (ScopedCollapsingHeader only draws the card at the top level).
class MenuCard
{
  public:
    MenuCard(const char* label, float menuResScale, bool open = true)
        : _header(label, open ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None)
    {
        ImGui::PushItemWidth(220.0f * menuResScale);
    }

    MenuCard(const MenuCard&) = delete;
    MenuCard& operator=(const MenuCard&) = delete;

    ~MenuCard() { ImGui::PopItemWidth(); }

    bool IsOpen() const { return _header.IsHeaderOpen(); }

  private:
    ScopedCollapsingHeader _header;
};
