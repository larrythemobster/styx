#include "Core/Gifs/FavoriteGifs.hpp"

#include <algorithm>
#include <chrono>
#include <span>

#include "Core/Logging.hpp"
#include "Discord/Client.hpp"
#include "Platform/Base64.hpp"
#include "Proto/ProtoReader.hpp"

namespace Styx::Core {
namespace {
constexpr auto RateLimitedRetry = std::chrono::seconds(30);
}

FavoriteGifs::FavoriteGifs(Discord::Client *client) : client(client) {}

std::vector<Proto::FavoriteGif> FavoriteGifs::newestFirst() const
{
    return FavoriteGifRules::newestFirst(favorites.gifs);
}

int FavoriteGifs::indexOf(const std::string &key) const
{
    const auto found = std::find_if(favorites.gifs.cbegin(), favorites.gifs.cend(), [&key](const Proto::FavoriteGif &gif) { return gif.url == key; });
    return found == favorites.gifs.cend() ? -1 : int(found - favorites.gifs.cbegin());
}

bool FavoriteGifs::contains(const std::string &url) const
{
    return indexOf(url) >= 0 || indexOf(FavoriteGifRules::keyFor(url)) >= 0;
}

void FavoriteGifs::add(const FavoriteGifCandidate &candidate)
{
    if (!loaded || !candidate.isValid())
        return;

    const Proto::FavoriteGif gif = FavoriteGifRules::stored(candidate, FavoriteGifRules::orderAfter(favorites.gifs));
    Proto::FavoriteGifs edited = favorites;
    const int existing = indexOf(gif.url);
    if (existing >= 0)
        edited.gifs[existing] = gif;
    else
        edited.gifs.push_back(gif);
    if (edited.gifs.size() > FavoriteGifRules::CountThatHidesTheTooltip)
        edited.hideTooltip = true;
    if (edited.toProto().size() > FavoriteGifRules::OfficialClientMaxSerializedBytes) {
        limitReached.publish();
        return;
    }
    favorites = std::move(edited);
    changed.publish();
    saveSoon();
}

void FavoriteGifs::remove(const std::string &url)
{
    if (!loaded)
        return;
    const int exact = indexOf(url);
    const int index = exact >= 0 ? exact : indexOf(FavoriteGifRules::keyFor(url));
    if (index < 0)
        return;
    favorites.gifs.erase(favorites.gifs.begin() + index);
    changed.publish();
    saveSoon();
}

void FavoriteGifs::toggle(const FavoriteGifCandidate &candidate)
{
    if (contains(candidate.url))
        remove(candidate.url);
    else
        add(candidate);
}

void FavoriteGifs::onFrecencySettingsReceived(const Proto::FrecencyUserSettings &settings)
{
    loaded = true;
    if (!unsavedEdits)
        replaceWith(settings.favoriteGifs.value_or(Proto::FavoriteGifs()));
}

void FavoriteGifs::onUserSettingsProtoUpdated(const Discord::UserSettingsProtoUpdate &event)
{
    if (!event.type.hasValue() || event.type.get() != Discord::UserSettingsProtoType::FRECENCY || !event.proto.hasValue())
        return;
    const auto decoded = Platform::fromBase64(event.proto.get());
    if (!decoded || decoded->empty() || unsavedEdits)
        return;
    Proto::ProtoReader reader(std::span<const uint8_t>(decoded->data(), decoded->size()));
    const auto settings = Proto::FrecencyUserSettings::fromProto(reader);
    const bool partial = event.partial.hasValue() && event.partial.get();
    if (settings.favoriteGifs)
        replaceWith(*settings.favoriteGifs);
    else if (!partial)
        replaceWith(Proto::FavoriteGifs());
}

void FavoriteGifs::replaceWith(const Proto::FavoriteGifs &fromServer)
{
    favorites = fromServer;
    changed.publish();
}

void FavoriteGifs::saveSoon()
{
    unsavedEdits = true;
    if (!saveInFlight && !retryTimer.active())
        save();
}

void FavoriteGifs::save()
{
    Proto::FrecencyUserSettings onlyFavorites;
    onlyFavorites.favoriteGifs = favorites;
    unsavedEdits = false;
    saveInFlight = true;
    Platform::Guard<FavoriteGifs> self(this);
    client->patchFrecencySettings(onlyFavorites.toProtoPartial(), std::nullopt, [self](const Discord::Client::FrecencyPatchResult &result) {
        if (!self)
            return;
        self->saveInFlight = false;
        if (result.rateLimited) {
            Core::log(Core::LogLevel::Warning, LogCore) << "FavoriteGifs: save rate limited, retrying";
            self->unsavedEdits = true;
            self->retryTimer.start(RateLimitedRetry, false, [self] { if (self) self->save(); });
            return;
        }
        if (self->unsavedEdits) {
            self->save();
            return;
        }
        if (!result.success) {
            Core::log(Core::LogLevel::Warning, LogCore) << "FavoriteGifs: save failed, reloading: " << result.error;
            self->reloadFromServer();
        }
    });
}

void FavoriteGifs::reloadFromServer()
{
    client->fetchFrecencySettings([](const Result<Proto::FrecencyUserSettings> &) {});
}

} // namespace Styx::Core
