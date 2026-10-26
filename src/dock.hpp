// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "monitor.hpp"
#include <QWidget>
#include <QPointer>
#include <QTimer>
#include <QWindow>
#include <array>
#include <memory>
#include <cstdint>

class QTableView;
class QLabel;
namespace modulator {
class MonitorModel;
class MonitorPanel final : public QWidget {
  std::unique_ptr<MonitorFeed> feed;
  QTimer timer;
  QPointer<QWidget> main_window;
  QPointer<QWindow> observed_main, observed_self;
  std::array<QMetaObject::Connection,4> lifecycle;
  MonitorModel *model = nullptr;
  QTableView *table = nullptr;
  QLabel *empty = nullptr;
  bool exposed = false, suspended = false, active = false;
  bool building = false, pending_activation = false;
  bool refreshing = false, deferred_clear = false, reconcile_after_refresh = false;
  uint64_t generation = 0;
  bool should_monitor() const;
  void reconcile();
  void watch_windows();
  void refresh();
  void build();
protected:
  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;
public:
  explicit MonitorPanel(std::unique_ptr<MonitorFeed> source, QWidget *main = nullptr);
  ~MonitorPanel() override;
  void set_exposed(bool visible);
  void suspend(bool paused);
  bool monitoring() const { return active; }
  QSize sizeHint() const override { return {510,330}; }
  QTableView *view() const { return table; }
#ifdef MODULATOR_TESTING
  uint64_t samples = 0, history_appends = 0, timer_callbacks = 0;
  void test_refresh() { refresh(); }
  uint64_t test_paints() const;
#endif
};
}
