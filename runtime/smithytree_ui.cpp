// The weapon tree panel over the smithy (step 2: the weapon under the cursor - its names by level, where it is made
// from, what is made from it). Its data: runtime/hle/smithytree.c. Drawn by the overlay (overlay.cpp) while the
// smithy's weapon list is open, over the left of the screen (where the smithy shows the weapon's model).
#include "imgui.h"

#include "hle/smithytree.h"

static const char *const TYPE_NAME[15] = {"대검", "한손검", "해머", "랜스", "헤비보우건", "", "라이트보우건", "태도",
                                          "슬래시액스", "건랜스", "활", "쌍검", "수렵피리", "조충곤", "차지액스"};

extern "C" void smithytree_draw(const SmithyView *v, float scale) {
    ImGuiIO &io = ImGui::GetIO();
    float w = io.DisplaySize.x, h = io.DisplaySize.y;
    ImGui::SetNextWindowPos(ImVec2(w * 0.015f, h * 0.17f));
    ImGui::SetNextWindowSize(ImVec2(w * 0.245f, 0));
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("##smithytree", nullptr, flags)) { ImGui::End(); return; }
    const ImVec4 gold(1.0f, 0.82f, 0.3f, 1), dim(0.65f, 0.65f, 0.65f, 1), here(1.0f, 0.95f, 0.55f, 1);
    ImGui::SetWindowFontScale(1.15f);
    ImGui::TextColored(gold, "%s 트리", v->type >= 0 && v->type < 15 ? TYPE_NAME[v->type] : "");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    if (v->parent) {
        ImGui::TextColored(dim, "%s", v->parent_name);
        ImGui::TextColored(dim, "  -> 에서 파생");
    }
    for (int k = 0; k < v->ntiers; k++) {
        const SmithyTier *t = &v->tier[k];
        bool on = (int)v->level >= t->low && (int)v->level <= t->top;
        if (k) ImGui::TextColored(dim, "     |");
        ImVec4 c = on ? here : ImVec4(1, 1, 1, 1);
        if (t->low == t->top) ImGui::TextColored(c, "%s Lv%d  %s", on ? ">" : "  ", t->low, t->name);
        else ImGui::TextColored(c, "%s Lv%d-%d  %s", on ? ">" : "  ", t->low, t->top, t->name);
        ImGui::SameLine();
        ImGui::TextColored(dim, "  공격 %d", t->attack);
    }
    if (v->nchildren) {
        ImGui::Spacing();
        ImGui::TextColored(gold, "파생");
        for (int k = 0; k < v->nchildren; k++) {
            const SmithyChild *ch = &v->child[k];
            ImGui::Text("  - %s", ch->name);
            ImGui::SameLine();
            ImGui::TextColored(dim, "  Lv%d, 공격 %d", ch->top, ch->attack);
        }
    }
    ImGui::Spacing();
    ImGui::TextColored(dim, "(미리보기, 커서: Lv%u)", v->level);
    (void)scale;
    ImGui::End();
}
