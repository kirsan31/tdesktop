/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/edit_caption_box.h"

#include "ui/widgets/input_fields.h"
#include "ui/image/image.h"
#include "ui/text_options.h"
#include "ui/special_buttons.h"
#include "media/clip/media_clip_reader.h"
#include "history/history.h"
#include "history/history_item.h"
#include "data/data_media_types.h"
#include "data/data_photo.h"
#include "data/data_document.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "core/event_filter.h"
#include "chat_helpers/message_field.h"
#include "chat_helpers/tabbed_panel.h"
#include "chat_helpers/tabbed_selector.h"
#include "chat_helpers/emoji_suggestions_widget.h"
#include "window/window_controller.h"
#include "layout.h"
#include "auth_session.h"
#include "apiwrap.h"
#include "styles/style_history.h"
#include "styles/style_boxes.h"
#include "styles/style_chat_helpers.h"

EditCaptionBox::EditCaptionBox(
	QWidget*,
	not_null<Window::Controller*> controller,
	not_null<HistoryItem*> item)
: _controller(controller)
, _msgId(item->fullId()) {
	Expects(item->media() != nullptr);
	Expects(item->media()->allowsEditCaption());

	QSize dimensions;
	auto image = (Image*)nullptr;
	DocumentData *doc = nullptr;

	const auto media = item->media();
	if (const auto photo = media->photo()) {
		_photo = true;
		dimensions = QSize(photo->width(), photo->height());
		image = photo->large();
	} else if (const auto document = media->document()) {
		image = document->thumbnail();
		dimensions = image
			? image->size()
			: document->dimensions;
		if (document->isAnimation()) {
			_gifw = document->dimensions.width();
			_gifh = document->dimensions.height();
			_animated = true;
		} else if (document->isVideoFile()) {
			_animated = true;
		} else {
			_doc = true;
		}
		doc = document;
	}
	const auto original = item->originalText();
	const auto editData = TextWithTags {
		original.text,
		ConvertEntitiesToTextTags(original.entities)
	};

	if (!_animated && (dimensions.isEmpty() || doc || !image)) {
		if (!image) {
			_thumbw = 0;
		} else {
			const auto tw = image->width(), th = image->height();
			if (tw > th) {
				_thumbw = (tw * st::msgFileThumbSize) / th;
			} else {
				_thumbw = st::msgFileThumbSize;
			}
			_thumbnailImage = image;
			_refreshThumbnail = [=] {
				const auto options = Images::Option::Smooth
					| Images::Option::RoundedSmall
					| Images::Option::RoundedTopLeft
					| Images::Option::RoundedTopRight
					| Images::Option::RoundedBottomLeft
					| Images::Option::RoundedBottomRight;
				_thumb = App::pixmapFromImageInPlace(Images::prepare(
					image->pix(_msgId).toImage(),
					_thumbw * cIntRetinaFactor(),
					0,
					options,
					st::msgFileThumbSize,
					st::msgFileThumbSize));
			};
		}

		if (doc) {
			const auto nameString = doc->isVoiceMessage()
				? lang(lng_media_audio)
				: doc->composeNameString();
			_name.setText(
				st::semiboldTextStyle,
				nameString,
				Ui::NameTextOptions());
			_status = formatSizeText(doc->size);
			_statusw = std::max(
				_name.maxWidth(),
				st::normalFont->width(_status));
			_isImage = doc->isImage();
			_isAudio = (doc->isVoiceMessage() || doc->isAudioFile());
		}
		if (_refreshThumbnail) {
			_refreshThumbnail();
		}
	} else {
		if (!image) {
			image = Image::BlankMedia();
		}
		auto maxW = 0, maxH = 0;
		const auto limitW = st::sendMediaPreviewSize;
		auto limitH = std::min(st::confirmMaxHeight, _gifh ? _gifh : INT_MAX);
		if (_animated) {
			maxW = std::max(dimensions.width(), 1);
			maxH = std::max(dimensions.height(), 1);
			if (maxW * limitH > maxH * limitW) {
				if (maxW < limitW) {
					maxH = maxH * limitW / maxW;
					maxW = limitW;
				}
			} else {
				if (maxH < limitH) {
					maxW = maxW * limitH / maxH;
					maxH = limitH;
				}
			}
			_thumbnailImage = image;
			_refreshThumbnail = [=] {
				const auto options = Images::Option::Smooth
					| Images::Option::Blurred;
				_thumb = image->pixNoCache(
					_msgId,
					maxW * cIntRetinaFactor(),
					maxH * cIntRetinaFactor(),
					options,
					maxW,
					maxH);
			};
			prepareGifPreview(doc);
		} else {
			maxW = dimensions.width();
			maxH = dimensions.height();
			_thumbnailImage = image;
			_refreshThumbnail = [=] {
				_thumb = image->pixNoCache(
					_msgId,
					maxW * cIntRetinaFactor(),
					maxH * cIntRetinaFactor(),
					Images::Option::Smooth,
					maxW,
					maxH);
			};
		}
		_refreshThumbnail();

		const auto resizeDimensions = [&](int &thumbWidth, int &thumbHeight, int &thumbX) {
			auto tw = thumbWidth, th = thumbHeight;
			if (!tw || !th) {
				tw = th = 1;
			}
			if (thumbWidth < st::sendMediaPreviewSize) {
				thumbWidth = (thumbWidth > 20) ? thumbWidth : 20;
			} else {
				thumbWidth = st::sendMediaPreviewSize;
			}
			const auto maxThumbHeight = std::min(int(std::round(1.5 * thumbWidth)), limitH);
			thumbHeight = int(std::round(th * float64(thumbWidth) / tw));
			if (thumbHeight > maxThumbHeight) {
				thumbWidth = int(std::round(thumbWidth * float64(maxThumbHeight) / thumbHeight));
				thumbHeight = maxThumbHeight;
				if (thumbWidth < 10) {
					thumbWidth = 10;
				}
			}
			thumbX = (st::boxWideWidth - thumbWidth) / 2;
		};

		if (doc && doc->isAnimation()) {
			resizeDimensions(_gifw, _gifh, _gifx);
		}
		limitH = std::min(st::confirmMaxHeight, _gifh ? _gifh : INT_MAX);

		_thumbw = _thumb.width();
		_thumbh = _thumb.height();
		// If thumb's and resized gif's sizes are equal,
		// Then just take made values.
		if (_thumbw == _gifw && _thumbh == _gifh) {
			_thumbx = (st::boxWideWidth - _thumbw) / 2;
		} else {
			resizeDimensions(_thumbw, _thumbh, _thumbx);
		}

		const auto prepareBasicThumb = _refreshThumbnail;
		const auto scaleThumbDown = [=] {
			_thumb = App::pixmapFromImageInPlace(_thumb.toImage().scaled(
				_thumbw * cIntRetinaFactor(),
				_thumbh * cIntRetinaFactor(),
				Qt::IgnoreAspectRatio,
				Qt::SmoothTransformation));
			_thumb.setDevicePixelRatio(cRetinaFactor());
		};
		_refreshThumbnail = [=] {
			prepareBasicThumb();
			scaleThumbDown();
		};
		scaleThumbDown();
	}
	Assert(_animated || _photo || _doc);

	_thumbnailImageLoaded = _thumbnailImage
		? _thumbnailImage->loaded()
		: true;
	subscribe(Auth().downloaderTaskFinished(), [=] {
		if (!_thumbnailImageLoaded
			&& _thumbnailImage
			&& _thumbnailImage->loaded()) {
			_thumbnailImageLoaded = true;
			_refreshThumbnail();
			update();
		}
		if (doc && doc->isAnimation() && doc->loaded() && !_gifPreview) {
			prepareGifPreview(doc);
		}
	});

	_field.create(
		this,
		st::confirmCaptionArea,
		Ui::InputField::Mode::MultiLine,
		langFactory(lng_photo_caption),
		editData);
	_field->setMaxLength(Global::CaptionLengthMax());
	_field->setSubmitSettings(Ui::InputField::SubmitSettings::Both);
	_field->setInstantReplaces(Ui::InstantReplaces::Default());
	_field->setInstantReplacesEnabled(Global::ReplaceEmojiValue());
	_field->setMarkdownReplacesEnabled(rpl::single(true));
	_field->setEditLinkCallback(DefaultEditLinkCallback(_field));
}

bool EditCaptionBox::emojiFilter(not_null<QEvent*> event) {
	const auto type = event->type();
	if (type == QEvent::Move || type == QEvent::Resize) {
		// updateEmojiPanelGeometry uses not only container geometry, but
		// also container children geometries that will be updated later.
		crl::on_main(this, [=] { updateEmojiPanelGeometry(); });
	}
	return false;
}

void EditCaptionBox::updateEmojiPanelGeometry() {
	const auto parent = _emojiPanel->parentWidget();
	const auto global = _emojiToggle->mapToGlobal({ 0, 0 });
	const auto local = parent->mapFromGlobal(global);
	_emojiPanel->moveBottomRight(
		local.y(),
		local.x() + _emojiToggle->width() * 3);
}

void EditCaptionBox::prepareGifPreview(not_null<DocumentData*> document) {
	if (_gifPreview) {
		return;
	} else if (document->isAnimation() && document->loaded()) {
		_gifPreview = Media::Clip::MakeReader(document, _msgId, [this](Media::Clip::Notification notification) {
			clipCallback(notification);
		});
		if (_gifPreview) _gifPreview->setAutoplay();
	}
}

void EditCaptionBox::clipCallback(Media::Clip::Notification notification) {
	using namespace Media::Clip;
	switch (notification) {
	case NotificationReinit: {
		if (_gifPreview && _gifPreview->state() == State::Error) {
			_gifPreview.setBad();
		}

		if (_gifPreview && _gifPreview->ready() && !_gifPreview->started()) {
			const auto s = QSize(_gifw, _gifh);
			_gifPreview->start(s.width(), s.height(), s.width(), s.height(), ImageRoundRadius::None, RectPart::None);
		}

		update();
	} break;

	case NotificationRepaint: {
		if (_gifPreview && !_gifPreview->currentDisplayed()) {
			update();
		}
	} break;
	}
}

void EditCaptionBox::prepare() {
	addButton(langFactory(lng_settings_save), [this] { save(); });
	addButton(langFactory(lng_cancel), [this] { closeBox(); });

	updateBoxSize();
	connect(_field, &Ui::InputField::submitted, [=] { save(); });
	connect(_field, &Ui::InputField::cancelled, [=] { closeBox(); });
	connect(_field, &Ui::InputField::resized, [=] { captionResized(); });
	Ui::Emoji::SuggestionsController::Init(
		getDelegate()->outerContainer(),
		_field);

	setupEmojiPanel();

	auto cursor = _field->textCursor();
	cursor.movePosition(QTextCursor::End);
	_field->setTextCursor(cursor);
}

void EditCaptionBox::captionResized() {
	updateBoxSize();
	resizeEvent(0);
	updateEmojiPanelGeometry();
	update();
}

void EditCaptionBox::setupEmojiPanel() {
	const auto container = getDelegate()->outerContainer();
	_emojiPanel = base::make_unique_q<ChatHelpers::TabbedPanel>(
		container,
		_controller,
		object_ptr<ChatHelpers::TabbedSelector>(
			nullptr,
			_controller,
			ChatHelpers::TabbedSelector::Mode::EmojiOnly));
	_emojiPanel->setDesiredHeightValues(
		1.,
		st::emojiPanMinHeight / 2,
		st::emojiPanMinHeight);
	_emojiPanel->hide();
	_emojiPanel->getSelector()->emojiChosen(
	) | rpl::start_with_next([=](EmojiPtr emoji) {
		Ui::InsertEmojiAtCursor(_field->textCursor(), emoji);
	}, lifetime());

	_emojiFilter.reset(Core::InstallEventFilter(
		container,
		[=](not_null<QEvent*> event) { return emojiFilter(event); }));

	_emojiToggle.create(this, st::boxAttachEmoji);
	_emojiToggle->installEventFilter(_emojiPanel);
	_emojiToggle->addClickHandler([=] {
		_emojiPanel->toggleAnimated();
	});
}

void EditCaptionBox::updateBoxSize() {
	auto newHeight = st::boxPhotoPadding.top() + st::boxPhotoCaptionSkip + _field->height() + errorTopSkip() + st::normalFont->height;
	if (_photo || _animated) {
		newHeight += std::max(_thumbh, _gifh);
	} else if (_thumbw) {
		newHeight += 0 + st::msgFileThumbSize + 0;
	} else if (_doc) {
		newHeight += 0 + st::msgFileSize + 0;
	} else {
		newHeight += st::boxTitleFont->height;
	}
	setDimensions(st::boxWideWidth, newHeight);
}

int EditCaptionBox::errorTopSkip() const {
	return (st::boxButtonPadding.top() / 2);
}

void EditCaptionBox::paintEvent(QPaintEvent *e) {
	BoxContent::paintEvent(e);

	Painter p(this);

	if (_photo || _animated) {
		const auto th = std::max(_gifh, _thumbh);
		if (_thumbx > st::boxPhotoPadding.left()) {
			p.fillRect(st::boxPhotoPadding.left(), st::boxPhotoPadding.top(), _thumbx - st::boxPhotoPadding.left(), th, st::confirmBg);
		}
		if (_thumbx + _thumbw < width() - st::boxPhotoPadding.right()) {
			p.fillRect(_thumbx + _thumbw, st::boxPhotoPadding.top(), width() - st::boxPhotoPadding.right() - _thumbx - _thumbw, th, st::confirmBg);
		}
		if (_gifPreview && _gifPreview->started()) {
			const auto s = QSize(_gifw, _gifh);
			const auto paused = _controller->isGifPausedAtLeastFor(Window::GifPauseReason::Layer);
			const auto frame = _gifPreview->current(s.width(), s.height(), s.width(), s.height(), ImageRoundRadius::None, RectPart::None, paused ? 0 : crl::now());
			p.drawPixmap(_gifx, st::boxPhotoPadding.top(), frame);
		} else {
			const auto offset = _gifh ? ((_gifh - _thumbh) / 2) : 0;
			p.drawPixmap(_thumbx, st::boxPhotoPadding.top() + offset, _thumb);
		}
		if (_animated && !_gifPreview) {
			QRect inner(_thumbx + (_thumbw - st::msgFileSize) / 2, st::boxPhotoPadding.top() + (th - st::msgFileSize) / 2, st::msgFileSize, st::msgFileSize);
			p.setPen(Qt::NoPen);
			p.setBrush(st::msgDateImgBg);

			{
				PainterHighQualityEnabler hq(p);
				p.drawEllipse(inner);
			}

			const auto icon = &st::historyFileInPlay;
			icon->paintInCenter(p, inner);
		}
	} else if (_doc) {
		const auto w = width() - st::boxPhotoPadding.left() - st::boxPhotoPadding.right();
		const auto h = _thumbw ? (0 + st::msgFileThumbSize + 0) : (0 + st::msgFileSize + 0);
		auto nameleft = 0, nametop = 0, nameright = 0, statustop = 0;
		if (_thumbw) {
			nameleft = 0 + st::msgFileThumbSize + st::msgFileThumbPadding.right();
			nametop = st::msgFileThumbNameTop - st::msgFileThumbPadding.top();
			nameright = 0;
			statustop = st::msgFileThumbStatusTop - st::msgFileThumbPadding.top();
		} else {
			nameleft = 0 + st::msgFileSize + st::msgFilePadding.right();
			nametop = st::msgFileNameTop - st::msgFilePadding.top();
			nameright = 0;
			statustop = st::msgFileStatusTop - st::msgFilePadding.top();
		}
		const auto namewidth = w - nameleft - 0;
		if (namewidth > _statusw) {
			//w -= (namewidth - _statusw);
			//namewidth = _statusw;
		}
		const auto x = (width() - w) / 2, y = st::boxPhotoPadding.top();

//		App::roundRect(p, x, y, w, h, st::msgInBg, MessageInCorners, &st::msgInShadow);

		if (_thumbw) {
			QRect rthumb(rtlrect(x + 0, y + 0, st::msgFileThumbSize, st::msgFileThumbSize, width()));
			p.drawPixmap(rthumb.topLeft(), _thumb);
		} else {
			const QRect inner(rtlrect(x + 0, y + 0, st::msgFileSize, st::msgFileSize, width()));
			p.setPen(Qt::NoPen);
			p.setBrush(st::msgFileInBg);

			{
				PainterHighQualityEnabler hq(p);
				p.drawEllipse(inner);
			}

			const auto icon = &(_isAudio ? st::historyFileInPlay : _isImage ? st::historyFileInImage : st::historyFileInDocument);
			icon->paintInCenter(p, inner);
		}
		p.setFont(st::semiboldFont);
		p.setPen(st::historyFileNameInFg);
		_name.drawLeftElided(p, x + nameleft, y + nametop, namewidth, width());

		const auto &status = st::mediaInFg;
		p.setFont(st::normalFont);
		p.setPen(status);
		p.drawTextLeft(x + nameleft, y + statustop, width(), _status);
	} else {
		p.setFont(st::boxTitleFont);
		p.setPen(st::boxTextFg);
		p.drawTextLeft(_field->x(), st::boxPhotoPadding.top(), width(), lang(lng_edit_message));
	}

	if (!_error.isEmpty()) {
		p.setFont(st::normalFont);
		p.setPen(st::boxTextFgError);
		p.drawTextLeft(_field->x(), _field->y() + _field->height() + errorTopSkip(), width(), _error);
	}
}

void EditCaptionBox::resizeEvent(QResizeEvent *e) {
	BoxContent::resizeEvent(e);
	_field->resize(st::sendMediaPreviewSize, _field->height());
	_field->moveToLeft(st::boxPhotoPadding.left(), height() - st::normalFont->height - errorTopSkip() - _field->height());
	_emojiToggle->moveToLeft(
		(st::boxPhotoPadding.left()
			+ st::sendMediaPreviewSize
			- _emojiToggle->width()),
		_field->y() + st::boxAttachEmojiTop);
}

void EditCaptionBox::setInnerFocus() {
	_field->setFocusFast();
}

void EditCaptionBox::save() {
	if (_saveRequestId) return;

	const auto item = App::histItemById(_msgId);
	if (!item) {
		_error = lang(lng_edit_deleted);
		update();
		return;
	}

	auto flags = MTPmessages_EditMessage::Flag::f_message | 0;
	if (_previewCancelled) {
		flags |= MTPmessages_EditMessage::Flag::f_no_webpage;
	}
	const auto textWithTags = _field->getTextWithAppliedMarkdown();
	auto sending = TextWithEntities{
		textWithTags.text,
		ConvertTextTagsToEntities(textWithTags.tags)
	};
	const auto prepareFlags = Ui::ItemTextOptions(
		item->history(),
		Auth().user()).flags;
	TextUtilities::PrepareForSending(sending, prepareFlags);
	TextUtilities::Trim(sending);

	const auto sentEntities = TextUtilities::EntitiesToMTP(
		sending.entities,
		TextUtilities::ConvertOption::SkipLocal);
	if (!sentEntities.v.isEmpty()) {
		flags |= MTPmessages_EditMessage::Flag::f_entities;
	}
	_saveRequestId = MTP::send(
		MTPmessages_EditMessage(
			MTP_flags(flags),
			item->history()->peer->input,
			MTP_int(item->id),
			MTP_string(sending.text),
			MTPInputMedia(),
			MTPReplyMarkup(),
			sentEntities),
		rpcDone(&EditCaptionBox::saveDone),
		rpcFail(&EditCaptionBox::saveFail));
}

void EditCaptionBox::saveDone(const MTPUpdates &updates) {
	_saveRequestId = 0;
	closeBox();
	Auth().api().applyUpdates(updates);
}

bool EditCaptionBox::saveFail(const RPCError &error) {
	if (MTP::isDefaultHandledError(error)) return false;

	_saveRequestId = 0;
	QString err = error.type();
	if (err == qstr("MESSAGE_ID_INVALID") || err == qstr("CHAT_ADMIN_REQUIRED") || err == qstr("MESSAGE_EDIT_TIME_EXPIRED")) {
		_error = lang(lng_edit_error);
	} else if (err == qstr("MESSAGE_NOT_MODIFIED")) {
		closeBox();
		return true;
	} else if (err == qstr("MESSAGE_EMPTY")) {
		_field->setFocus();
		_field->showError();
	} else {
		_error = lang(lng_edit_error);
	}
	update();
	return true;
}
