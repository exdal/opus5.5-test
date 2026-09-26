// HUD and menus through RmlUi. Every Oxylus scene owns an RmlView (its own Rml::Context, renderer and input
// routing), so the game just loads documents into the scene's context and binds one data model to them.

#include <RmlUi/Core.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <glm/common.hpp>

#include "Core/App.hpp"
#include "Core/VFS.hpp"
#include "Render/Window.hpp"
#include "Scene/Components.hpp"
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

  // settings: the panel replaces the menu's buttons while it's open; every toggle is saved straight away
  constructor.Bind("settings_open", &h.settings_open);
  constructor.Bind("sfx_on", &h.sfx_on);
  constructor.Bind("music_on", &h.music_on);
  constructor.BindEventCallback("open_settings", [&self](Rml::DataModelHandle model, Rml::Event&, const Rml::VariantList&) {
    self.hud.settings_open = true;
    model.DirtyVariable("settings_open");
  });
  constructor.BindEventCallback("close_settings", [&self](Rml::DataModelHandle model, Rml::Event&, const Rml::VariantList&) {
    self.hud.settings_open = false;
    model.DirtyVariable("settings_open");
  });
  constructor.BindEventCallback("toggle_sfx", [&self](Rml::DataModelHandle model, Rml::Event&, const Rml::VariantList&) {
    self.hud.sfx_on = !self.hud.sfx_on;
    model.DirtyVariable("sfx_on");
    self.save_settings();
  });
  constructor.BindEventCallback("toggle_music", [&self](Rml::DataModelHandle model, Rml::Event&, const Rml::VariantList&) {
    self.hud.music_on = !self.hud.music_on;
    model.DirtyVariable("music_on");
    self.save_settings();
  });
  // multiplayer: the menu's panel, the scoreboard, the kill feed and the name tags over other players' heads
  constructor.Bind("online", &h.online);
  constructor.Bind("join_open", &h.join_open);
  constructor.Bind("join_address", &h.join_address);
  constructor.Bind("player_name", &h.player_name);
  constructor.Bind("host_port", &h.host_port);
  constructor.Bind("net_status", &h.net_status);
  if (auto tag = constructor.RegisterStruct<NameTag>()) {
    tag.RegisterMember("name", &NameTag::name);
    tag.RegisterMember("x", &NameTag::x);
    tag.RegisterMember("y", &NameTag::y);
    tag.RegisterMember("stars", &NameTag::stars);
  }
  constructor.RegisterArray<std::vector<NameTag>>();
  constructor.Bind("tags", &h.tags);
  if (auto row = constructor.RegisterStruct<ScoreRow>()) {
    row.RegisterMember("name", &ScoreRow::name);
    row.RegisterMember("score", &ScoreRow::score);
    row.RegisterMember("kills", &ScoreRow::kills);
    row.RegisterMember("stars", &ScoreRow::stars);
    row.RegisterMember("me", &ScoreRow::me);
  }
  constructor.RegisterArray<std::vector<ScoreRow>>();
  constructor.Bind("scoreboard", &h.scoreboard);
  constructor.RegisterArray<std::vector<Rml::String>>();
  constructor.Bind("kill_feed", &h.kill_feed);
  constructor.BindEventCallback("open_join", [&self](Rml::DataModelHandle model, Rml::Event&, const Rml::VariantList&) {
    self.hud.join_open = true;
    model.DirtyVariable("join_open");
  });
  constructor.BindEventCallback("close_join", [&self](Rml::DataModelHandle model, Rml::Event&, const Rml::VariantList&) {
    self.hud.join_open = false;
    model.DirtyVariable("join_open");
  });
  constructor.BindEventCallback("join_game", [&self](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    self.join_requested = true;
    self.save_settings();
  });
  constructor.BindEventCallback("host_game", [&self](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    self.host_requested = true;
    self.hud.join_open = false;
    self.save_settings();
  });
  constructor.BindEventCallback("leave_game", [&self](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    self.leave_requested = true;
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

auto World::pager_to(this World& self, PlayerID id, std::string_view message) -> void {
  if (self.is_local(id)) {
    self.pager(message);
  } else if (id != PlayerID::Invalid) {
    self.record({.kind = net::EventKind::Pager, .target = static_cast<u8>(id), .text = std::string(message)});
  }
}

auto World::update_hud(this World& self) -> void {
  auto& h = self.hud;
  const auto dt = 1.0f / 60.0f;
  self.pager_timer = glm::max(0.0f, self.pager_timer - dt);

  const auto* me = self.local_player();
  if (!me) {
    return;
  }
  // the money counter rolls up like an old arcade score instead of jumping
  const auto diff = me->cash - h.money;
  if (diff != 0) {
    const auto step = glm::max(1, glm::abs(diff) / 12);
    h.money += diff > 0 ? glm::min(step, diff) : glm::max(-step, diff);
  }
  h.wanted = self.stars(self.local);
  h.health = static_cast<i32>(glm::ceil(me->health));
  h.ammo = me->ammo;
  h.weapon = weapon_name(me->weapon);
  h.pager = self.pager_timer > 0.0f ? self.pager_text : "";
  h.prompt = self.state == GameState::Playing && me->life == Life::Alive ? self.prompt_text : "";
  if (me->car != CarID::Invalid) {
    const auto& c = self.car(me->car);
    h.vehicle = c.display_name;
    h.speed = static_cast<i32>(glm::length(self.car_velocity(me->car)) * 3.6f);
  } else {
    h.vehicle = "";
    h.speed = 0;
  }
  // your own FLATLINED / ARRESTED, whatever the rest of the city is up to
  switch (me->life) {
    case Life::Dead    : h.big_text = me->life_note.starts_with("WASTED") ? "WASTED" : "FLATLINED"; break;
    case Life::Arrested: h.big_text = "ARRESTED"; break;
    default            : h.big_text = ""; break;
  }
  if (me->life != Life::Alive) {
    h.stats = me->life_note;
  } else if (self.state == GameState::Paused || self.state == GameState::MainMenu) {
    h.stats = fmt::format(
      "BANKS ROBBED {}  |  CARS STOLEN {}  |  PEOPLE ROBBED {}  |  BODY COUNT {}",
      me->stats.banks_robbed,
      me->stats.cars_stolen,
      me->stats.peds_robbed,
      me->stats.peds_killed
    );
  }

  self.update_multiplayer_hud();

  // cheap enough for a dozen scalars, and it can't forget one
  self.hud_model->DirtyAllVariables();
}

auto World::update_multiplayer_hud(this World& self) -> void {
  auto& h = self.hud;
  h.online = self.role != NetRole::Offline;
  h.kill_feed.clear();
  for (const auto& line : self.kill_feed) {
    h.kill_feed.emplace_back(line.text);
  }

  h.scoreboard.clear();
  h.tags.clear();
  if (!h.online) {
    return;
  }
  for (auto id : ALL_PLAYERS) {
    const auto& p = self.pl(id);
    if (p.active) {
      h.scoreboard.push_back({.name = p.name, .score = p.score, .kills = p.kills, .stars = self.stars(id), .me = self.is_local(id)});
    }
  }
  std::ranges::sort(h.scoreboard, [](const ScoreRow& a, const ScoreRow& b) { return a.score > b.score; });

  // name tags: every other player's position through last frame's camera, the same matrices the cursor ray uses
  const auto* camera = self.camera.try_get<ox::CameraComponent>();
  if (!camera || self.headless) {
    return;
  }
  const auto screen = glm::vec2(ox::App::get_window().get_logical_size());
  const auto view_projection = camera->matrices.projection_matrix * camera->matrices.view_matrix;
  for (auto id : ALL_PLAYERS) {
    const auto& p = self.pl(id);
    if (!p.active || self.is_local(id) || p.name.empty()) {
      continue;
    }
    const auto clip = view_projection * glm::vec4(to3(self.player_position(id), p.car != CarID::Invalid ? 2.6f : 2.3f), 1.0f);
    if (clip.w <= 0.001f) {
      continue;
    }
    const auto ndc = glm::vec2(clip) / clip.w;
    if (glm::abs(ndc.x) > 1.1f || glm::abs(ndc.y) > 1.1f) {
      continue;
    }
    // (inverse of Camera::get_screen_ray: screen y grows with ndc y)
    const auto pixel = (ndc * 0.5f + 0.5f) * screen;
    h.tags.push_back({.name = p.name, .x = pixel.x, .y = pixel.y - 10.0f, .stars = self.stars(id)});
  }
}
} // namespace oxcity
