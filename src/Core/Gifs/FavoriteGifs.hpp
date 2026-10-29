#pragma once

#include <string>
#include <vector>

#include "Core/Gifs/FavoriteGifRules.hpp"
#include "Discord/Events.hpp"
#include "Platform/Timer.hpp"
#include "Platform/Trackable.hpp"
#include "Proto/FrecencySettings.hpp"

namespace Styx {
namespace Discord { class Client; }
namespace Core {

class FavoriteGifs : public Platform::Trackable
{
public:
    explicit FavoriteGifs(Discord::Client *client);

    [[nodiscard]] bool isLoaded() const { return loaded; }
    [[nodiscard]] std::vector<Proto::FavoriteGif> newestFirst() const;
    [[nodiscard]] bool contains(const std::string &url) const;
    void add(const FavoriteGifCandidate &candidate);
    void remove(const std::string &url);
    void toggle(const FavoriteGifCandidate &candidate);
    void onFrecencySettingsReceived(const Proto::FrecencyUserSettings &settings);
    void onUserSettingsProtoUpdated(const Discord::UserSettingsProtoUpdate &event);

    Platform::Signal<> changed;
    Platform::Signal<> limitReached;

private:
    [[nodiscard]] int indexOf(const std::string &key) const;
    void replaceWith(const Proto::FavoriteGifs &fromServer);
    void saveSoon();
    void save();
    void reloadFromServer();

    Discord::Client *client;
    Proto::FavoriteGifs favorites;
    Platform::Timer retryTimer;
    bool loaded = false;
    bool unsavedEdits = false;
    bool saveInFlight = false;
};

} // namespace Core
} // namespace Styx
