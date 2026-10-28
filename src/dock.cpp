// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "dock.hpp"
#include "sample-history.hpp"
#include <QAbstractTableModel>
#include <QApplication>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QShowEvent>
#include <QScopeGuard>
#ifdef MODULATOR_DOCK_TRACE
#include <util/base.h>
#endif
#include <QStyledItemDelegate>
#include <QStyleOptionButton>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTableView>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <chrono>

namespace modulator {
namespace {
QString text(const std::string &s) { return QString::fromUtf8(s.c_str()); }
QString timing(const MonitorRow &r) {
  if(!r.timed)return {};
  QString result=QString("Phase left: %1s").arg(r.phase_remaining,0,'f',1);
  if(r.one_shot)result+=QString(" · Shot left: %1s").arg(r.total_remaining,0,'f',1);
  return result;
}
QString number(double v, bool integer) { return QString::number(v, integer ? 'f':'g', integer ? 0:5); }
struct Entry {
  MonitorRow row;
  SampleHistory history;
};
bool same(const MonitorRow &a, const MonitorRow &b) {
  return a.id==b.id && a.source==b.source && a.name==b.name && a.target==b.target &&
    a.property==b.property && a.reason==b.reason && a.state==b.state && a.enabled==b.enabled &&
    a.available==b.available && a.integer==b.integer && a.value==b.value && a.low==b.low &&
    a.high==b.high && a.waveform==b.waveform && a.one_shot==b.one_shot && a.timed==b.timed &&
    a.progress==b.progress && a.phase_remaining==b.phase_remaining && a.total_remaining==b.total_remaining;
}
}
class MonitorModel final : public QAbstractTableModel {
public:
  std::vector<Entry> entries;
  MonitorFeed &feed;
  bool writable = false;
  double history_now = 0;
#ifdef MODULATOR_TESTING
  mutable uint64_t paints = 0;
#endif
  std::function<void()> changed;
  explicit MonitorModel(MonitorFeed &f, QObject *parent) : QAbstractTableModel(parent), feed(f) {}
  int rowCount(const QModelIndex &p = {}) const override { return p.isValid() ? 0:static_cast<int>(entries.size()); }
  int columnCount(const QModelIndex &p = {}) const override { return p.isValid() ? 0:4; }
  QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
    if(orientation!=Qt::Horizontal || role!=Qt::DisplayRole)return {};
    const char *headers[] = {"On", "Mapping / state", "Target value", "Play"};
    return section>=0 && section<4 ? QVariant(headers[section]):QVariant{};
  }
  QVariant data(const QModelIndex &idx, int role) const override {
    if(!idx.isValid() || idx.row()>=rowCount())return {};
    const auto &r=entries[static_cast<size_t>(idx.row())].row;
    if(role==Qt::CheckStateRole && idx.column()==0)return r.enabled ? Qt::Checked:Qt::Unchecked;
    if((role==Qt::ToolTipRole || role==Qt::AccessibleTextRole) && idx.column()==3)
      return r.one_shot ? QVariant("Play / retrigger one shot"):QVariant("Continuous mode: use the eye or Restart cycle");
    if(role==Qt::ToolTipRole || role==Qt::AccessibleTextRole) {
      return text(r.source)+" → "+text(r.target)+" → "+text(r.property)+"\n"+text(r.name)+" · "+
        monitor_wave_name(r.waveform)+"\n"+monitor_state_name(r.state)+"\n"+text(r.reason)+
        "\nTarget value: "+(r.available ? number(r.value,r.integer):QString("Unavailable"))+
        " · Range: "+number(r.low,r.integer)+" to "+number(r.high,r.integer)+"\n"+timing(r);
    }
    if(role==Qt::DisplayRole) {
      if(idx.column()==1)return text(r.source)+" / "+text(r.name)+" · "+monitor_state_name(r.state);
      if(idx.column()==3)return r.one_shot ? QVariant("Play"):QVariant{};
      if(idx.column()==2)return r.available ? number(r.value,r.integer):QString("Unavailable");
    }
    return {};
  }
  Qt::ItemFlags flags(const QModelIndex &idx) const override {
    auto result=QAbstractTableModel::flags(idx);
    if(idx.column()==0 && writable)result|=Qt::ItemIsUserCheckable;
    return result;
  }
  bool setData(const QModelIndex &idx, const QVariant &value, int role) override {
    if(!writable || !idx.isValid() || idx.row()>=rowCount())return false;
    const auto row=entries[static_cast<size_t>(idx.row())].row;
    if(idx.column()==3 && role==Qt::EditRole) {
      if(!row.one_shot || !row.available || !feed.play(row.id))return false;
    } else if(idx.column()!=0 || role!=Qt::CheckStateRole || !feed.set_enabled(row.id,value.toInt()==Qt::Checked))return false;
    if(changed)changed();
    return true;
  }
  void clear() { beginResetModel();entries.clear();endResetModel(); }
  uint64_t update(std::vector<MonitorRow> rows, QTableView *view) {
    history_now=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    bool topology=rows.size()!=entries.size();
    if(!topology)for(size_t i=0;i<rows.size();++i)if(rows[i].id!=entries[i].row.id){topology=true;break;}
    if(topology) {
      beginResetModel();std::vector<Entry> replacement;replacement.reserve(rows.size());
      for(auto &r:rows) {
        auto old=std::find_if(entries.begin(),entries.end(),[&](const Entry &e){return e.row.id==r.id;});
        Entry e;if(old!=entries.end())e=std::move(*old);e.row=std::move(r);replacement.push_back(std::move(e));
      }
      entries=std::move(replacement);endResetModel();
    }
    uint64_t appended=0;
    for(size_t i=0;i<entries.size();++i) {
      auto &e=entries[i];bool dirty=topology, mapping=topology, enabled=topology;
      if(!topology) {
        dirty=!same(e.row,rows[i]);
        const auto &a=e.row;const auto &b=rows[i];
        enabled=a.enabled!=b.enabled;
        mapping=a.source!=b.source || a.name!=b.name || a.target!=b.target || a.property!=b.property ||
          a.reason!=b.reason || a.state!=b.state || a.waveform!=b.waveform || a.one_shot!=b.one_shot ||
          a.timed!=b.timed || a.progress!=b.progress || a.phase_remaining!=b.phase_remaining || a.total_remaining!=b.total_remaining;
        if(e.row.target!=rows[i].target || e.row.property!=rows[i].property || e.row.integer!=rows[i].integer)
          e.history.clear();
        e.row=std::move(rows[i]);
      }
      const auto rect=view->visualRect(index(static_cast<int>(i),2));
      const bool visible=!rect.isEmpty() && rect.intersects(view->viewport()->rect());
      if(visible && e.row.available){if(e.history.append(e.row.value,history_now))++appended;dirty=true;}
      else if(e.history.count){e.history.clear();dirty=true;}
      if(!topology) {
        const int row=static_cast<int>(i);
        // Animation changes only the value/history cell. Leave static labels,
        // checkboxes and headers out of the regular repaint damage region.
        if(dirty)emit dataChanged(index(row,2),index(row,2));
        if(mapping){emit dataChanged(index(row,1),index(row,1));emit dataChanged(index(row,3),index(row,3));}
        if(enabled)emit dataChanged(index(row,0),index(row,0));
      }
    }
    return appended;
  }
};
namespace {
class MonitorDelegate final : public QStyledItemDelegate {
public:
  explicit MonitorDelegate(QObject *parent) : QStyledItemDelegate(parent) {}
  bool editorEvent(QEvent *event,QAbstractItemModel *model,const QStyleOptionViewItem &o,const QModelIndex &idx) override {
    if(idx.column()==3) {
      const bool mouse=event->type()==QEvent::MouseButtonRelease &&
        static_cast<QMouseEvent *>(event)->button()==Qt::LeftButton;
      const bool key=event->type()==QEvent::KeyPress &&
        (static_cast<QKeyEvent *>(event)->key()==Qt::Key_Space || static_cast<QKeyEvent *>(event)->key()==Qt::Key_Return);
      return (mouse || key) && model->setData(idx,true,Qt::EditRole);
    }
    return QStyledItemDelegate::editorEvent(event,model,o,idx);
  }
  void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &idx) const override {
#ifdef MODULATOR_TESTING
    ++static_cast<const MonitorModel *>(idx.model())->paints;
#endif
    if(idx.column()==0){QStyledItemDelegate::paint(p,o,idx);return;}
    const auto *m=static_cast<const MonitorModel *>(idx.model());
    const auto &e=m->entries[static_cast<size_t>(idx.row())];const auto &r=e.row;
    if(idx.column()==3) {
      if(r.one_shot) {
        QStyleOptionButton button;button.rect=o.rect.adjusted(4,20,-4,-20);button.text="Play";
        button.state=QStyle::State_Raised;
        if(r.available && m->writable)button.state|=QStyle::State_Enabled;
        if(o.state&QStyle::State_HasFocus)button.state|=QStyle::State_HasFocus;
        QApplication::style()->drawControl(QStyle::CE_PushButton,&button,p);
      }
      return;
    }
    p->save();p->setClipRect(o.rect);
    const bool selected=o.state&QStyle::State_Selected;
    p->fillRect(o.rect,selected ? o.palette.highlight():o.palette.base());
    const QColor ink=selected ? o.palette.highlightedText().color():o.palette.text().color();
    QColor faint=ink;faint.setAlpha(155);QColor accent=o.palette.highlight().color();
    if(selected)accent=ink;
    auto area=o.rect.adjusted(6,3,-6,-3);p->setPen(ink);
    if(idx.column()==1) {
      auto f=o.font;f.setBold(true);p->setFont(f);
      p->drawText(area.x(),area.y(),area.width(),19,Qt::AlignVCenter,o.fontMetrics.elidedText(text(r.name),Qt::ElideRight,area.width()));
      p->setFont(o.font);p->setPen(faint);
      const QString route=text(r.source)+" → "+text(r.target)+" / "+text(r.property);
      p->drawText(area.x(),area.y()+21,area.width(),18,Qt::AlignVCenter,o.fontMetrics.elidedText(route,Qt::ElideMiddle,area.width()));
      p->setPen(ink);
      const QString state=QString(monitor_state_name(r.state))+" · "+monitor_wave_name(r.waveform);
      p->drawText(area.x(),area.y()+43,area.width(),18,Qt::AlignVCenter,o.fontMetrics.elidedText(state,Qt::ElideRight,area.width()));
      if(r.timed) {
        p->setPen(faint);p->drawText(area.x(),area.y()+61,area.width(),17,Qt::AlignVCenter,
          o.fontMetrics.elidedText(timing(r),Qt::ElideRight,area.width()));
        p->fillRect(QRectF(area.x(),area.bottom()-2,area.width()*r.progress,2),accent);
      }
    } else {
      p->drawText(area.x(),area.y(),area.width(),18,Qt::AlignVCenter,r.available ? number(r.value,r.integer):QString("Unavailable"));
      const bool bounds=std::isfinite(r.low) && std::isfinite(r.high) && r.low<=r.high;
      const double span=r.high-r.low;
      auto normalized=[&](double v){return span>0 ? std::clamp((v-r.low)/span,0.0,1.0):.5;};
      if(bounds && r.available) {
        const int y=area.y()+26;p->setPen(faint);p->drawLine(area.x(),y,area.right(),y);
        p->setPen(Qt::NoPen);p->setBrush(accent);
        p->drawEllipse(QPointF(area.x()+normalized(r.value)*area.width(),y),3,3);
        if(r.value<r.low || r.value>r.high){p->setPen(ink);p->drawText(area,Qt::AlignTop|Qt::AlignRight,"↗");}
        p->setPen(faint);auto small=o.font;small.setPointSizeF(std::max(8.0,small.pointSizeF()-2));p->setFont(small);
        p->drawText(area.x(),area.y()+31,area.width(),14,Qt::AlignLeft,number(r.low,r.integer));
        p->drawText(area.x(),area.y()+31,area.width(),14,Qt::AlignRight,number(r.high,r.integer));
        if(e.history.count>1) {
          QPainterPath path;
          for(int i=0;i<e.history.count;++i) {
            const auto &sample=e.history.at(i);
            const QPointF point(area.right()-std::clamp(m->history_now-sample.time,0.0,10.0)*area.width()/10.0,
                area.bottom()-normalized(sample.value)*14);
            if(i==0 || sample.time-e.history.at(i-1).time>.25)path.moveTo(point);else path.lineTo(point);
          }
          p->setRenderHint(QPainter::Antialiasing);p->setPen(QPen(accent,1));p->drawPath(path);
        }
      }
    }
    p->restore();
  }
};
}
MonitorPanel::MonitorPanel(std::unique_ptr<MonitorFeed> source, QWidget *main)
  : QWidget(main), feed(std::move(source)), main_window(main)
{
  setObjectName("modulator-monitor-panel");setMinimumSize(360,180);resize(510,330);
  timer.setInterval(67);timer.setTimerType(Qt::CoarseTimer);
  connect(&timer,&QTimer::timeout,this,[this]{
#ifdef MODULATOR_TESTING
    ++timer_callbacks;
#endif
    if(active)refresh();
  });
}
MonitorPanel::~MonitorPanel() { timer.stop();feed->clear(); }
#ifdef MODULATOR_TESTING
uint64_t MonitorPanel::test_paints() const { return model ? model->paints:0; }
#endif
void MonitorPanel::build()
{
  if(model)return;
  auto *layout=new QVBoxLayout(this);layout->setContentsMargins(4,4,4,4);
  empty=new QLabel("No modulators in this collection. Add an LFO Modulator filter to a source.",this);
  empty->setWordWrap(true);layout->addWidget(empty);
  model=new MonitorModel(*feed,this);model->changed=[this]{if(active)refresh();};
  table=new QTableView(this);table->setModel(model);table->setItemDelegate(new MonitorDelegate(table));
  table->setSelectionBehavior(QAbstractItemView::SelectRows);table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);table->setShowGrid(false);
  table->verticalHeader()->hide();table->verticalHeader()->setDefaultSectionSize(94);
  table->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Fixed);table->setColumnWidth(0,32);
  table->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Stretch);
  table->horizontalHeader()->setSectionResizeMode(2,QHeaderView::Fixed);table->setColumnWidth(2,145);
  table->horizontalHeader()->setSectionResizeMode(3,QHeaderView::Fixed);table->setColumnWidth(3,62);
  layout->addWidget(table);
  auto *help=new QLabel("Target settings · Off may still ease out · History covers the last 10 seconds",this);
  help->setWordWrap(true);layout->addWidget(help);
}
void MonitorPanel::refresh()
{
  if(!active || refreshing)return;
  refreshing=true;
  const auto finish=qScopeGuard([this]{
    refreshing=false;
    if(deferred_clear){deferred_clear=false;if(model)model->clear();feed->clear();}
    if(reconcile_after_refresh){reconcile_after_refresh=false;QTimer::singleShot(0,this,[this]{reconcile();});}
  });
#ifdef MODULATOR_TESTING
  ++samples;
#endif
  const auto ticket=generation;
  auto rows=feed->sample();
  if(!active || ticket!=generation)return;
  const auto appended=model->update(std::move(rows),table);
  if(!active || ticket!=generation)return;
#ifdef MODULATOR_TESTING
  history_appends+=appended;
#else
  (void)appended;
#endif
  const bool none=model->rowCount()==0;empty->setVisible(none);table->setVisible(!none);
}
void MonitorPanel::reconcile()
{
#ifdef MODULATOR_DOCK_TRACE
  blog(LOG_INFO,"Modulator gate: exposed=%d suspended=%d widget=%d main=%d minimized=%d active=%d pending=%d model=%d",
    exposed,suspended,isVisible(),main_window ? main_window->isVisible():true,window()->isMinimized(),active,pending_activation,model!=nullptr);
#endif
  if(building)return;
  if(refreshing) {
    reconcile_after_refresh=true;
    if(!should_monitor()) {
      if(active){active=false;++generation;timer.stop();}
      if(model)model->writable=false;
      deferred_clear=true;
    }
    return;
  }
  watch_windows();
  const bool visible=should_monitor();
  if(visible && !model) {
    if(pending_activation)return;
    pending_activation=true;const auto ticket=++generation;
    // OBS styles/places a floating dock inside visibilityChanged. Building its
    // layout synchronously there can reenter hide/show and leave it dormant.
    QTimer::singleShot(0,this,[this,ticket]{
      if(ticket!=generation)return;
      pending_activation=false;
      if(!should_monitor())return;
      building=true;build();building=false;reconcile();
    });
    return;
  }
  if(!visible && pending_activation){pending_activation=false;++generation;}
  if(visible==active)return;
  active=visible;
  if(active){model->writable=true;refresh();if(active)timer.start();
#ifdef MODULATOR_DOCK_TRACE
    blog(LOG_INFO,"Modulator activated: rows=%d active=%d",model->rowCount(),active);
#endif
  }
  else {
    ++generation;
    timer.stop();if(model){model->writable=false;model->clear();}feed->clear();
  }
}
bool MonitorPanel::should_monitor() const
{
  return exposed && !suspended && isVisible() && !window()->isMinimized() &&
    (!main_window || (main_window->isVisible() && !main_window->isMinimized()));
}
void MonitorPanel::set_exposed(bool visible) { exposed=visible;reconcile(); }
void MonitorPanel::suspend(bool paused) { suspended=paused;reconcile(); }
// OBS blocks QDockWidget signals when its Docks-menu action calls setVisible.
// Content show/hide must therefore also own this gate. The dock signal remains
// necessary for tab exposure, where QWidget::isVisible alone is insufficient.
void MonitorPanel::showEvent(QShowEvent *e) { QWidget::showEvent(e);exposed=true;reconcile(); }
void MonitorPanel::hideEvent(QHideEvent *e) { exposed=false;reconcile();QWidget::hideEvent(e); }
void MonitorPanel::watch_windows()
{
  QWindow *main=exposed && !suspended && main_window ? main_window->windowHandle():nullptr;
  QWindow *self=exposed && !suspended ? window()->windowHandle():nullptr;
  if(self==main)self=nullptr;
  if(main==observed_main && self==observed_self)return;
  for(auto &c:lifecycle){QObject::disconnect(c);c={};}
  observed_main=main;observed_self=self;
  if(main) {
    lifecycle[0]=connect(main,&QWindow::windowStateChanged,this,[this]{reconcile();});
    lifecycle[1]=connect(main,&QWindow::visibilityChanged,this,[this]{reconcile();});
  }
  if(self) {
    lifecycle[2]=connect(self,&QWindow::windowStateChanged,this,[this]{reconcile();});
    lifecycle[3]=connect(self,&QWindow::visibilityChanged,this,[this]{reconcile();});
  }
}
}
