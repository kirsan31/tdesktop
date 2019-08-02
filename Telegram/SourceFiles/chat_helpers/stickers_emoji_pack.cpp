/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_helpers/stickers_emoji_pack.h"

#include "history/history_item.h"
#include "ui/emoji_config.h"
#include "ui/text/text_isolated_emoji.h"
#include "ui/image/image_source.h"
#include "main/main_session.h"
#include "data/data_file_origin.h"
#include "data/data_session.h"
#include "data/data_document.h"
#include "base/concurrent_timer.h"
#include "apiwrap.h"
#include "styles/style_history.h"

namespace Stickers {
namespace details {

class EmojiImageLoader {
public:
	EmojiImageLoader(
		crl::weak_on_queue<EmojiImageLoader> weak,
		int id);

	[[nodiscard]] QImage prepare(const IsolatedEmoji &emoji);
	void switchTo(int id);

private:
	crl::weak_on_queue<EmojiImageLoader> _weak;
	std::optional<Ui::Emoji::UniversalImages> _images;

	base::ConcurrentTimer _unloadTimer;

};

namespace {

constexpr auto kRefreshTimeout = TimeId(7200);
constexpr auto kUnloadTimeout = 86400 * crl::time(1000);

[[nodiscard]] QSize CalculateSize(const IsolatedEmoji &emoji) {
	using namespace rpl::mappers;

	const auto single = st::largeEmojiSize;
	const auto skip = st::largeEmojiSkip;
	const auto outline = st::largeEmojiOutline;
	const auto count = ranges::count_if(emoji.items, _1 != nullptr);
	const auto items = single * count + skip * (count - 1);
	return QSize(
		2 * outline + items,
		2 * outline + single
	) * cIntRetinaFactor();
}

class ImageSource : public Images::Source {
public:
	explicit ImageSource(
		const IsolatedEmoji &emoji,
		not_null<crl::object_on_queue<EmojiImageLoader>*> loader);

	void load(Data::FileOrigin origin) override;
	void loadEvenCancelled(Data::FileOrigin origin) override;
	QImage takeLoaded() override;
	void unload() override;

	void automaticLoad(
		Data::FileOrigin origin,
		const HistoryItem *item) override;
	void automaticLoadSettingsChanged() override;

	bool loading() override;
	bool displayLoading() override;
	void cancel() override;
	float64 progress() override;
	int loadOffset() override;

	const StorageImageLocation &location() override;
	void refreshFileReference(const QByteArray &data) override;
	std::optional<Storage::Cache::Key> cacheKey() override;
	void setDelayedStorageLocation(
		const StorageImageLocation &location) override;
	void performDelayedLoad(Data::FileOrigin origin) override;
	bool isDelayedStorageImage() const override;
	void setImageBytes(const QByteArray &bytes) override;

	int width() override;
	int height() override;
	int bytesSize() override;
	void setInformation(int size, int width, int height) override;

	QByteArray bytesForCache() override;

private:
	// While HistoryView::Element-s are almost never destroyed
	// we make loading of the image lazy.
	not_null<crl::object_on_queue<EmojiImageLoader>*> _loader;
	IsolatedEmoji _emoji;
	QImage _data;
	QByteArray _format;
	QByteArray _bytes;
	QSize _size;
	base::binary_guard _loading;

};

ImageSource::ImageSource(
	const IsolatedEmoji &emoji,
	not_null<crl::object_on_queue<EmojiImageLoader>*> loader)
: _loader(loader)
, _emoji(emoji)
, _size(CalculateSize(emoji)) {
}

void ImageSource::load(Data::FileOrigin origin) {
	if (!_data.isNull()) {
		return;
	}
	if (_bytes.isEmpty()) {
		_loader->with([
			this,
			emoji = _emoji,
			guard = _loading.make_guard()
		](EmojiImageLoader &loader) mutable {
			if (!guard) {
				return;
			}
			crl::on_main(std::move(guard), [this, image = loader.prepare(emoji)]{
				_data = image;
				Auth().downloaderTaskFinished().notify();
			});
		});
	} else {
		_data = App::readImage(_bytes, &_format, false);
	}
}

void ImageSource::loadEvenCancelled(Data::FileOrigin origin) {
	load(origin);
}

QImage ImageSource::takeLoaded() {
	load({});
	return _data;
}

void ImageSource::unload() {
	if (_bytes.isEmpty() && !_data.isNull()) {
		if (_format != "JPG") {
			_format = "PNG";
		}
		{
			QBuffer buffer(&_bytes);
			_data.save(&buffer, _format);
		}
		Assert(!_bytes.isEmpty());
	}
	_data = QImage();
}

void ImageSource::automaticLoad(
	Data::FileOrigin origin,
	const HistoryItem *item) {
}

void ImageSource::automaticLoadSettingsChanged() {
}

bool ImageSource::loading() {
	return _data.isNull() && _bytes.isEmpty();
}

bool ImageSource::displayLoading() {
	return false;
}

void ImageSource::cancel() {
}

float64 ImageSource::progress() {
	return 1.;
}

int ImageSource::loadOffset() {
	return 0;
}

const StorageImageLocation &ImageSource::location() {
	return StorageImageLocation::Invalid();
}

void ImageSource::refreshFileReference(const QByteArray &data) {
}

std::optional<Storage::Cache::Key> ImageSource::cacheKey() {
	return std::nullopt;
}

void ImageSource::setDelayedStorageLocation(
	const StorageImageLocation &location) {
}

void ImageSource::performDelayedLoad(Data::FileOrigin origin) {
}

bool ImageSource::isDelayedStorageImage() const {
	return false;
}

void ImageSource::setImageBytes(const QByteArray &bytes) {
}

int ImageSource::width() {
	return _size.width();
}

int ImageSource::height() {
	return _size.height();
}

int ImageSource::bytesSize() {
	return _bytes.size();
}

void ImageSource::setInformation(int size, int width, int height) {
	if (width && height) {
		_size = QSize(width, height);
	}
}

QByteArray ImageSource::bytesForCache() {
	auto result = QByteArray();
	{
		QBuffer buffer(&result);
		if (!_data.save(&buffer, _format)) {
			if (_data.save(&buffer, "PNG")) {
				_format = "PNG";
			}
		}
	}
	return result;
}

} // namespace

EmojiImageLoader::EmojiImageLoader(
	crl::weak_on_queue<EmojiImageLoader> weak,
	int id)
: _weak(std::move(weak))
, _images(std::in_place, id)
, _unloadTimer(_weak.runner(), [=] { _images->clear(); }) {
}

QImage EmojiImageLoader::prepare(const IsolatedEmoji &emoji) {
	Expects(_images.has_value());

	_images->ensureLoaded();
	auto result = QImage(
		CalculateSize(emoji),
		QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);
	{
		QPainter p(&result);
		auto x = st::largeEmojiOutline;
		const auto y = st::largeEmojiOutline;
		for (const auto &single : emoji.items) {
			if (!single) {
				break;
			}
			_images->draw(
				p,
				single,
				st::largeEmojiSize * cIntRetinaFactor(),
				x,
				y);
			x += st::largeEmojiSize + st::largeEmojiSkip;
		}
	}
	_unloadTimer.callOnce(kUnloadTimeout);
	return result;
}

void EmojiImageLoader::switchTo(int id) {
	_images.emplace(id);
}

} // namespace details

EmojiPack::EmojiPack(not_null<Main::Session*> session)
: _session(session)
, _imageLoader(Ui::Emoji::CurrentSetId()) {
	refresh();

	session->data().itemRemoved(
	) | rpl::filter([](not_null<const HistoryItem*> item) {
		return item->isIsolatedEmoji();
	}) | rpl::start_with_next([=](not_null<const HistoryItem*> item) {
		remove(item);
	}, _lifetime);

	session->settings().largeEmojiChanges(
	) | rpl::start_with_next([=] {
		refreshAll();
	}, _lifetime);

	Ui::Emoji::Updated(
	) | rpl::start_with_next([=] {
		const auto id = Ui::Emoji::CurrentSetId();
		_images.clear();
		_imageLoader.with([=](details::EmojiImageLoader &loader) {
			loader.switchTo(id);
		});
		refreshAll();
	}, _lifetime);
}

EmojiPack::~EmojiPack() = default;

bool EmojiPack::add(not_null<HistoryItem*> item) {
	auto length = 0;
	if (const auto emoji = item->isolatedEmoji()) {
		_items[emoji].emplace(item);
		return true;
	}
	return false;
}

void EmojiPack::remove(not_null<const HistoryItem*> item) {
	Expects(item->isIsolatedEmoji());

	auto length = 0;
	const auto emoji = item->isolatedEmoji();
	const auto i = _items.find(emoji);
	Assert(i != end(_items));
	const auto j = i->second.find(item);
	Assert(j != end(i->second));
	i->second.erase(j);
	if (i->second.empty()) {
		_items.erase(i);
	}
}

DocumentData *EmojiPack::stickerForEmoji(const IsolatedEmoji &emoji) {
	Expects(!emoji.empty());

	if (emoji.items[1] != nullptr) {
		return nullptr;
	}
	const auto i = _map.find(emoji.items[0]);
	return (i != end(_map)) ? i->second.get() : nullptr;
}

std::shared_ptr<Image> EmojiPack::image(const IsolatedEmoji &emoji) {
	const auto i = _images.emplace(emoji, std::weak_ptr<Image>()).first;
	if (const auto result = i->second.lock()) {
		return result;
	}
	auto result = std::make_shared<Image>(
		std::make_unique<details::ImageSource>(emoji, &_imageLoader));
	i->second = result;
	return result;
}

void EmojiPack::refresh() {
	if (_requestId) {
		return;
	}
	_requestId = _session->api().request(MTPmessages_GetStickerSet(
		MTP_inputStickerSetAnimatedEmoji()
	)).done([=](const MTPmessages_StickerSet &result) {
		_requestId = 0;
		refreshDelayed();
		result.match([&](const MTPDmessages_stickerSet &data) {
			applySet(data);
		});
	}).fail([=](const RPCError &error) {
		_requestId = 0;
		refreshDelayed();
	}).send();
}

void EmojiPack::applySet(const MTPDmessages_stickerSet &data) {
	const auto stickers = collectStickers(data.vdocuments().v);
	auto was = base::take(_map);

	for (const auto &pack : data.vpacks().v) {
		pack.match([&](const MTPDstickerPack &data) {
			applyPack(data, stickers);
		});
	}

	for (const auto &[emoji, document] : _map) {
		const auto i = was.find(emoji);
		if (i == end(was)) {
			refreshItems(emoji);
		} else {
			if (i->second != document) {
				refreshItems(i->first);
			}
			was.erase(i);
		}
	}
	for (const auto &[emoji, Document] : was) {
		refreshItems(emoji);
	}
}

void EmojiPack::refreshAll() {
	for (const auto &[emoji, list] : _items) {
		refreshItems(list);
	}
}

void EmojiPack::refreshItems(EmojiPtr emoji) {
	const auto i = _items.find(IsolatedEmoji{ { emoji } });
	if (i == end(_items)) {
		return;
	}
	refreshItems(i->second);
}

void EmojiPack::refreshItems(
		const base::flat_set<not_null<HistoryItem*>> &list) {
	for (const auto &item : list) {
		_session->data().requestItemViewRefresh(item);
	}
}

void EmojiPack::applyPack(
		const MTPDstickerPack &data,
		const base::flat_map<uint64, not_null<DocumentData*>> &map) {
	const auto emoji = [&] {
		return Ui::Emoji::Find(qs(data.vemoticon()));
	}();
	const auto document = [&]() -> DocumentData * {
		for (const auto &id : data.vdocuments().v) {
			const auto i = map.find(id.v);
			if (i != end(map)) {
				return i->second.get();
			}
		}
		return nullptr;
	}();
	if (emoji && document) {
		_map.emplace_or_assign(emoji, document);
	}
}

base::flat_map<uint64, not_null<DocumentData*>> EmojiPack::collectStickers(
		const QVector<MTPDocument> &list) const {
	auto result = base::flat_map<uint64, not_null<DocumentData*>>();
	for (const auto &sticker : list) {
		const auto document = _session->data().processDocument(
			sticker);
		if (document->sticker()) {
			result.emplace(document->id, document);
		}
	}
	return result;
}

void EmojiPack::refreshDelayed() {
	App::CallDelayed(details::kRefreshTimeout, _session, [=] {
		refresh();
	});
}

} // namespace Stickers