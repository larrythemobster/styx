#include "ClientInstance.hpp"
#include "PrivateChannels.hpp"
#include "ReadStateManager.hpp"
#include "Discord/GroupDm.hpp"
#include "Storage/DatabaseManager.hpp"
#include "Storage/Transaction.hpp"

#include <algorithm>
#include <utility>

namespace Styx::Core {
namespace {

bool privateType(Discord::ChannelType type)
{
    return type == Discord::ChannelType::DM || type == Discord::ChannelType::GROUP_DM;
}

bool validId(Snowflake id)
{
    return id.isValid() && id != Snowflake(0);
}

} // namespace

void ClientInstance::createGroupDm(const std::vector<Snowflake> &recipients,
                                   Discord::Client::ResultCallback<Discord::Channel> callback)
{
    client->createGroupDm(recipients, [this, guard = Platform::Guard(this), generation = sessionGeneration,
                                      callback = std::move(callback)](const Result<Discord::Channel> &result) {
        if (!guard)
            return;
        if (generation != sessionGeneration) {
            if (callback) callback(Result<Discord::Channel>::makeError("Account session changed"));
            return;
        }
        if (!result.success()) {
            if (callback) callback(result);
            return;
        }
        auto stored = ingestCreatedGroup(*result.value);
        if (guard && callback)
            callback(stored);
    });
}

void ClientInstance::addDmRecipient(Snowflake channelId, Snowflake userId,
                                    Discord::Client::ResultCallback<std::optional<Discord::Channel>> callback)
{
    if (!availablePrivateChannel(channelId, false)) {
        if (callback) callback(Result<std::optional<Discord::Channel>>::makeError("Private channel is unavailable"));
        return;
    }
    const auto version = privateChannelLifecycles[channelId];
    client->addDmRecipient(channelId, userId,
        [this, guard = Platform::Guard(this), generation = sessionGeneration, version, channelId, userId,
         callback = std::move(callback)](const Result<std::optional<Discord::Channel>> &result) {
        if (!guard)
            return;
        if (generation != sessionGeneration) {
            if (callback) callback(Result<std::optional<Discord::Channel>>::makeError("Account session changed"));
            return;
        }
        if (!result.success()) {
            if (callback) callback(result);
            return;
        }
        if (result.value->has_value()) {
            auto stored = ingestCreatedGroup(**result.value);
            if (guard && callback) {
                if (stored.success())
                    callback(Result<std::optional<Discord::Channel>>::makeOk(std::move(*stored.value)));
                else
                    callback(Result<std::optional<Discord::Channel>>::makeError(std::move(stored.error), stored.code));
            }
            return;
        }
        auto current = availablePrivateChannel(channelId, true);
        if (!current) {
            if (callback) callback(Result<std::optional<Discord::Channel>>::makeError("Group DM is unavailable"));
            return;
        }
        const auto latest = privateChannelLifecycles[channelId];
        if (latest.lifetime == version.lifetime && latest.revision == version.revision && userId != account.id) {
            if (applyPrivateRecipient(*current, userId, true)) {
                ensureUserStub(userId);
                applyPrivateChannel(std::move(current));
            }
        }
        if (guard && callback)
            callback(Result<std::optional<Discord::Channel>>::makeOk(std::nullopt));
    });
}

void ClientInstance::removeGroupDmRecipient(Snowflake channelId, Snowflake userId,
                                            Discord::Client::ActionCallback callback)
{
    if (!availablePrivateChannel(channelId, true)) {
        if (callback) callback(Result<void>::makeError("Group DM is unavailable"));
        return;
    }
    const auto version = privateChannelLifecycles[channelId];
    client->removeGroupDmRecipient(channelId, userId,
        [this, guard = Platform::Guard(this), generation = sessionGeneration, version, channelId, userId,
         callback = std::move(callback)](const Result<void> &result) {
        if (!guard)
            return;
        if (generation != sessionGeneration) {
            if (callback) callback(Result<void>::makeError("Account session changed"));
            return;
        }
        if (!result.success()) {
            if (callback) callback(result);
            return;
        }
        auto current = availablePrivateChannel(channelId, true);
        if (!current) {
            if (callback) callback(Result<void>::makeError("Group DM is unavailable"));
            return;
        }
        const auto latest = privateChannelLifecycles[channelId];
        if (latest.lifetime == version.lifetime && latest.revision == version.revision) {
            if (userId == account.id)
                deletePrivateChannel(channelId);
            else if (applyPrivateRecipient(*current, userId, false))
                applyPrivateChannel(std::move(current));
        }
        if (guard && callback)
            callback(Result<void>::makeOk());
    });
}

void ClientInstance::modifyGroupDm(Snowflake channelId, const Discord::GroupDmEdit &edit,
                                   Discord::Client::ResultCallback<Discord::Channel> callback)
{
    if (!availablePrivateChannel(channelId, true)) {
        if (callback) callback(Result<Discord::Channel>::makeError("Group DM is unavailable"));
        return;
    }
    const auto version = privateChannelLifecycles[channelId];
    client->modifyGroupDm(channelId, edit,
        [this, guard = Platform::Guard(this), generation = sessionGeneration, version, channelId,
         callback = std::move(callback)](const Result<Discord::Channel> &result) {
        if (!guard)
            return;
        if (generation != sessionGeneration) {
            if (callback) callback(Result<Discord::Channel>::makeError("Account session changed"));
            return;
        }
        if (!result.success()) {
            if (callback) callback(result);
            return;
        }
        auto current = availablePrivateChannel(channelId, true);
        if (!current || privateChannelLifecycles[channelId].lifetime != version.lifetime) {
            if (callback) callback(Result<Discord::Channel>::makeError("Group DM is unavailable"));
            return;
        }
        if (privateChannelLifecycles[channelId].revision == version.revision)
            *current = applyPrivateChannel(std::move(current), &*result.value);
        if (guard && callback)
            callback(Result<Discord::Channel>::makeOk(std::move(*current)));
    });
}

void ClientInstance::leaveGroupDm(Snowflake channelId, bool silent,
                                  Discord::Client::ResultCallback<Discord::Channel> callback)
{
    if (!availablePrivateChannel(channelId, true)) {
        if (callback) callback(Result<Discord::Channel>::makeError("Group DM is unavailable"));
        return;
    }
    const auto version = privateChannelLifecycles[channelId];
    client->leaveGroupDm(channelId, silent,
        [this, guard = Platform::Guard(this), generation = sessionGeneration, version, channelId,
         callback = std::move(callback)](const Result<Discord::Channel> &result) {
        if (!guard)
            return;
        if (generation != sessionGeneration) {
            if (callback) callback(Result<Discord::Channel>::makeError("Account session changed"));
            return;
        }
        if (!result.success()) {
            if (callback) callback(result);
            return;
        }
        const auto latest = privateChannelLifecycles[channelId];
        if (!latest.deleted && latest.lifetime == version.lifetime)
            deletePrivateChannel(channelId);
        if (guard && callback)
            callback(result);
    });
}

std::optional<Discord::Channel> ClientInstance::availablePrivateChannel(Snowflake id, bool groupOnly)
{
    const auto version = privateChannelLifecycles.find(id);
    if (version != privateChannelLifecycles.end() && version->second.deleted)
        return std::nullopt;
    auto channel = channelRepo.getChannel(id);
    if (!channel || !privateType(channel->type.get()) ||
        (groupOnly && channel->type.get() != Discord::ChannelType::GROUP_DM))
        return std::nullopt;
    return channel;
}

Result<Discord::Channel> ClientInstance::ingestCreatedGroup(const Discord::Channel &channel)
{
    const auto id = channel.id.get();
    auto &version = privateChannelLifecycles[id];
    if (version.deleted)
        return Result<Discord::Channel>::makeError("Group DM is unavailable");
    if (auto existing = channelRepo.getChannel(id)) {
        if (existing->type.get() != Discord::ChannelType::GROUP_DM)
            return Result<Discord::Channel>::makeError("Group DM is unavailable");
        return Result<Discord::Channel>::makeOk(std::move(*existing));
    }
    ++version.lifetime;
    return Result<Discord::Channel>::makeOk(applyPrivateChannel(std::nullopt, &channel));
}

void ClientInstance::deletePrivateChannel(Snowflake channelId)
{
    Discord::ChannelDelete event;
    event.id = channelId;
    onChannelDeleted(event);
}

bool ClientInstance::handlePrivateChannelCreated(const Discord::Channel &channel)
{
    if (!privateType(channel.type.get()))
        return false;
    if (!validId(channel.id.get()))
        return true;
    auto existing = channelRepo.getChannel(channel.id.get());
    auto &version = privateChannelLifecycles[channel.id.get()];
    if (!existing || version.deleted)
        ++version.lifetime;
    version.deleted = false;
    applyPrivateChannel(std::move(existing), &channel);
    return true;
}

bool ClientInstance::handlePrivateChannelUpdated(const Discord::Channel &channel)
{
    auto existing = channelRepo.getChannel(channel.id.get());
    if (!existing)
        return true;
    if (!privateType(existing->type.get()))
        return false;
    auto version = privateChannelLifecycles.find(channel.id.get());
    if (version == privateChannelLifecycles.end() || !version->second.deleted)
        applyPrivateChannel(std::move(existing), &channel);
    return true;
}

void ClientInstance::onChannelRecipientAdded(const Discord::ChannelRecipient &event)
{
    onPrivateRecipient(event, true);
}

void ClientInstance::onChannelRecipientRemoved(const Discord::ChannelRecipient &event)
{
    onPrivateRecipient(event, false);
}

void ClientInstance::onPrivateRecipient(const Discord::ChannelRecipient &event, bool added)
{
    const auto id = event.channelId.get();
    const auto userId = event.user->id.get();
    if (!validId(id) || !validId(userId))
        return;
    if (userId == account.id) {
        if (!added) {
            auto known = channelRepo.getChannel(id);
            if (!known || known->type.get() == Discord::ChannelType::GROUP_DM)
                deletePrivateChannel(id);
        }
        return;
    }
    auto current = availablePrivateChannel(id, true);
    if (!current)
        return;
    cachePrivateUser(event.user.get());
    if (applyPrivateRecipient(*current, userId, added))
        applyPrivateChannel(std::move(current));
    else
        ++privateChannelLifecycles[id].revision;
}

void ClientInstance::cachePrivateUser(const Discord::User &user)
{
    if (!validId(user.id.get()) || user.id.get() == account.id)
        return;
    auto baseline = userManager->getUser(user.id.get());
    if (baseline)
        userManager->saveUser(mergePrivateUser(std::move(*baseline), user));
    else
        userManager->saveUser(user);
}

void ClientInstance::ensureUserStub(Snowflake userId)
{
    if (!userManager->getUser(userId)) {
        Discord::User stub;
        stub.id = userId;
        stub.identityFieldsPresent = 0;
        userManager->saveUser(stub);
    }
}

Discord::Channel ClientInstance::persistPrivateChannel(std::optional<Discord::Channel> baseline,
    const Discord::Channel *update, Platform::Database &db)
{
    auto merged = baseline ? std::move(*baseline) : Discord::Channel{};
    if (update) {
        if (!baseline) {
            merged.id = update->id;
            merged.type = update->type;
        }
        merged = mergePrivateChannel(std::move(merged), *update);
    }
    if (merged.recipientIds.hasValue()) {
        auto &ids = merged.recipientIds.get();
        std::erase(ids, account.id);
        if (update && update->recipientIds.hasValue() && !update->recipients.hasValue()) {
            for (auto id : ids)
                ensureUserStub(id);
        }
    }
    if (update && update->recipients.hasValue()) {
        for (const auto &user : update->recipients.get())
            cachePrivateUser(user);
    }
    channelRepo.saveChannel(merged, db);
    if (merged.permissionOverwrites.hasValue())
        channelRepo.savePermissionOverwrites(merged.id.get(), merged.permissionOverwrites.get(), db);
    if (merged.recipientIds.hasValue())
        channelRepo.saveChannelRecipients(merged.id.get(), merged.recipientIds.get(), db);
    return merged;
}

Discord::Channel ClientInstance::applyPrivateChannel(std::optional<Discord::Channel> baseline,
    const Discord::Channel *update)
{
    const bool created = !baseline;
    auto &db = Storage::DatabaseManager::instance().getDatabase(
        Storage::DatabaseManager::getCacheConnectionName(account.id));
    Storage::Transaction transaction(db);
    auto merged = persistPrivateChannel(std::move(baseline), update, db);
    transaction.commit();
    ++privateChannelLifecycles[merged.id.get()].revision;
    auto guard = Platform::Guard(this);
    readStateManager->registerPrivateChannel(merged);
    if (!guard)
        return merged;
    readStateChanged.publish(merged.id.get());
    if (!guard)
        return merged;
    if (created) {
        Discord::ChannelCreate event;
        event.channel = merged;
        channelCreated.publish(event);
    } else {
        Discord::ChannelUpdate event;
        event.channel = merged;
        channelUpdated.publish(event);
    }
    return merged;
}

} // namespace Styx::Core
