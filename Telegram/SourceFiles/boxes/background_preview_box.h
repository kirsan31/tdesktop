/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "boxes/abstract_box.h"
#include "base/binary_guard.h"
#include "window/themes/window_theme.h"
#include "history/admin_log/history_admin_log_item.h"
#include "history/view/history_view_element.h"
#include "ui/effects/radial_animation.h"

namespace Ui {
class Checkbox;
} // namespace Ui

class BackgroundPreviewBox
	: public BoxContent
	, public HistoryView::ElementDelegate {
public:
	BackgroundPreviewBox(QWidget*, const Data::WallPaper &paper);

	static bool Start(
		const QString &slug,
		const QMap<QString, QString> &params);

	using Element = HistoryView::Element;
	HistoryView::Context elementContext() override;
	std::unique_ptr<Element> elementCreate(
		not_null<HistoryMessage*> message) override;
	std::unique_ptr<Element> elementCreate(
		not_null<HistoryService*> message) override;
	bool elementUnderCursor(not_null<const Element*> view) override;
	void elementAnimationAutoplayAsync(
		not_null<const Element*> element) override;
	TimeMs elementHighlightTime(
		not_null<const Element*> element) override;
	bool elementInSelectionMode() override;

protected:
	void prepare() override;

	void paintEvent(QPaintEvent *e) override;

private:
	void apply();
	void share();
	void step_radial(TimeMs ms, bool timer);
	QRect radialRect() const;

	void checkLoadedDocument();
	bool setScaledFromThumb();
	void setScaledFromImage(QImage &&image, QImage &&blurred);
	void updateServiceBg(std::optional<QColor> background);
	std::optional<QColor> patternBackgroundColor() const;
	void paintImage(Painter &p, TimeMs ms);
	void paintRadial(Painter &p, TimeMs ms);
	void paintTexts(Painter &p, TimeMs ms);
	void paintDate(Painter &p);
	void createBlurCheckbox();
	int textsTop() const;
	void startFadeInFrom(QPixmap previous);
	void checkBlurAnimationStart();

	AdminLog::OwnedItem _text1;
	AdminLog::OwnedItem _text2;
	Data::WallPaper _paper;
	QImage _full;
	QPixmap _scaled, _blurred, _fadeOutThumbnail;
	Animation _fadeIn;
	Ui::RadialAnimation _radial;
	base::binary_guard _generating;
	std::optional<QColor> _serviceBg;
	object_ptr<Ui::Checkbox> _blur = { nullptr };

};
