// HUD and menus through RmlUi. Every Oxylus scene owns an RmlView (its own Rml::Context, renderer and input
// routing), so the game just loads documents into the scene's context and binds one data model to them.

#include <RmlUi/Core.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <glm/common.hpp>

#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Scene/Scene.hpp"
#include "Utils/Log.hpp"
#include "World.hpp"

namespace oxcity {
static auto ui_path(std::string_view relative) -> std::string {
  // RmlUi reads files through its default file interface (plain fopen), the engine doesn't install one that
  // goes through the vfs, so hand it real paths
  return ox::App::get_vfs().resolve_physical_dir(ox::VFS::APP_DIR, std::string(relative)).string();
}

static auto weapon_name(Weapon w) -> Rml::String {
  switch (w) {
    case Weapon::Fists : return "FISTS";
    case Weapon::Knife : return "KNIFE";
    case Weapon::Pistol: return "PISTOL";
  }
  return "";
}

auto World::init_hud(this World& self) -> bool {
  ZoneScoped;

  for (const auto* font : {"Fonts/FiraSans-Regular.ttf", "Fonts/FiraSans-Bold.ttf"}) {
    if (!Rml::LoadFontFace(ui_path(font))) {
      OX_LOG_ERROR("OxCity: couldn't load font {}", font);
      return false;
    }
  }

  auto* context = self.scene->get_rml_context();
  if (!context) {
    OX_LOG_ERROR("OxCity: the scene has no RmlUi context, is the RmlUI module registered?");
    return false;
  }

  auto constructor = context->CreateDataModel("hud");
  if (!constructor) {
    OX_LOG_ERROR("OxCity: couldn't create the hud data model");
    return false;
  }
  auto& h = self.hud;
  constructor.Bind("money", &h.money);
  constructor.Bind("wanted", &h.wanted);
  constructor.Bind("health", &h.health);
  constructor.Bind("ammo", &h.ammo);
  constructor.Bind("weapon", &h.weapon);
  constructor.Bind("vehicle", &h.vehicle);
  constructor.Bind("pager", &h.pager);
  constructor.Bind("prompt", &h.prompt);
  constructor.Bind("big_text", &h.big_text);
  constructor.Bind("heist_progress", &h.heist_progress);
  constructor.Bind("heist_active", &h.heist_active);
  constructor.Bind("menu_visible", &h.menu_visible);
  constructor.Bind("paused", &h.paused);
  constructor.Bind("speed", &h.speed);
  constructor.Bind("stats", &h.stats);
  constructor.Bind("kill_flash", &h.kill_flash);
  constructor.Bind("combo", &h.combo);
  constructor.Bind("combo_scale", &h.combo_scale);
  constructor.Bind("combo_tilt", &h.combo_tilt);
  constructor.Bind("score", &h.score);
  constructor.Bind("score_popup", &h.score_popup);
  constructor.BindEventCallback("start", [&self](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    self.start_requested = true;
  });
  constructor.BindEventCallback("quit", [&self](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    self.quit_requested = true;
  });
  self.hud_model = std::make_unique<Rml::DataModelHandle>(constructor.GetModelHandle());

  self.hud_document = context->LoadDocument(ui_path("UI/hud.rml"));
  self.menu_document = context->LoadDocument(ui_path("UI/menu.rml"));
  if (!self.hud_document || !self.menu_document) {
    OX_LOG_ERROR("OxCity: couldn't load the RmlUi documents");
    return false;
  }
  self.hud_document->Show();
  self.menu_document->Show();
  return true;
}

auto World::pager(this World& self, std::string_view message) -> void {
  self.pager_text = std::string(message);
  self.pager_timer = 6.0f;
  self.play(self.assets.sfx_pager, 0.5f);
}

auto World::update_hud(this World& self) -> void {
  auto& h = self.hud;
  const auto dt = 1.0f / 60.0f;
  self.pager_timer = glm::max(0.0f, self.pager_timer - dt);

  // the money counter rolls up like an old arcade score instead of jumping
  const auto diff = self.player.cash - h.money;
  if (diff != 0) {
    const auto step = glm::max(1, glm::abs(diff) / 12);
    h.money += diff > 0 ? glm::min(step, diff) : glm::max(-step, diff);
  }
  h.wanted = self.stars();
  h.health = static_cast<i32>(glm::ceil(self.player.health));
  h.ammo = self.player.ammo;
  h.weapon = weapon_name(self.player.weapon);
  h.pager = self.pager_timer > 0.0f ? self.pager_text : "";
  h.prompt = self.state == GameState::Playing ? self.prompt_text : "";
  if (self.player.car != CarID::Invalid) {
    const auto& c = self.car(self.player.car);
    h.vehicle = c.display_name;
    h.speed = static_cast<i32>(glm::length(self.car_velocity(self.player.car)) * 3.6f);
  } else {
    h.vehicle = "";
    h.speed = 0;
  }
  if (self.state == GameState::Paused || self.state == GameState::MainMenu) {
    h.stats = fmt::format(
      "BANKS ROBBED {}  |  CARS STOLEN {}  |  PEOPLE ROBBED {}  |  BODY COUNT {}",
      self.stats.banks_robbed,
      self.stats.cars_stolen,
      self.stats.peds_robbed,
      self.stats.peds_killed
    );
  }

  // cheap enough for a dozen scalars, and it can't forget one
  self.hud_model->DirtyAllVariables();
}
} // namespace oxcity
