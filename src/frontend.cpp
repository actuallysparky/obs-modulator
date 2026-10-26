// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "frontend.hpp"
#include "dock.hpp"
#include <obs-frontend-api.h>
#include <QApplication>
#include <QDockWidget>
#include <QPointer>
#include <QAction>
#include <QTableView>

namespace modulator {
namespace {
constexpr auto DockID="obs-modulator-live-monitor";
QPointer<MonitorPanel> panel;
bool registered=false;
void event(enum obs_frontend_event e, void *) {
  // OBS destroys its frontend API immediately after EXIT, before libobs
  // unloads modules. Remove our callback/widget while that API still exists.
  if(e==OBS_FRONTEND_EVENT_EXIT){shutdown_frontend();return;}
  if(!panel)return;
  if(e==OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING || e==OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP ||
     e==OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN)panel->suspend(true);
  if(e==OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED || e==OBS_FRONTEND_EVENT_FINISHED_LOADING)panel->suspend(false);
}
}
void install_frontend()
{
  // Headless libobs users have no QApplication/frontend callbacks. Never call
  // frontend APIs there: existing actual-plugin tests keep their original path.
  if(registered || !qobject_cast<QApplication *>(QCoreApplication::instance()))return;
  auto *main=static_cast<QWidget *>(obs_frontend_get_main_window());
  if(!main)return;
  panel=new MonitorPanel(make_monitor_feed(),main);
  if(!obs_frontend_add_dock_by_id(DockID,"Modulator Monitor",panel)){delete panel;return;}
  registered=true;
  blog(LOG_INFO,"[obs-modulator] Monitor dock registered (dormant until shown)");
  if(auto *dock=qobject_cast<QDockWidget *>(panel->parentWidget())) {
    QObject::connect(dock,&QDockWidget::visibilityChanged,panel,[](bool visible){if(panel)panel->set_exposed(visible);});
    auto *content=panel.data();
    // Only an explicit menu action raises/focuses the monitor. Restoring OBS's
    // saved layout or unminimizing must not steal the operator's focus.
    QObject::connect(dock->toggleViewAction(),&QAction::triggered,content,[dock,content](bool checked){
      if(!checked)return;
      dock->raise();dock->activateWindow();
      QTimer::singleShot(0,content,[content]{
        if(content->monitoring() && content->view())content->view()->setFocus(Qt::OtherFocusReason);
      });
    });
  }
  // setWidget on an already-realized OBS dock can leave the content explicitly
  // hidden. Showing the child does not show its hidden dock ancestor.
  panel->show();
  obs_frontend_add_event_callback(event,nullptr);
}
void shutdown_frontend()
{
  if(!registered)return;
  if(panel)panel->suspend(true);
  obs_frontend_remove_event_callback(event,nullptr);
  obs_frontend_remove_dock(DockID);panel=nullptr;registered=false;
}
}
