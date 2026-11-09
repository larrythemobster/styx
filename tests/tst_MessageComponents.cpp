#include "Discord/Entities.hpp"
#include "Discord/Enums.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace Styx::Discord;
using Styx::Core::Snowflake;

// Synthetic protocol fixtures based on documented Discord message components specification:
// https://docs.discord.food/resources/components

static Message parseMessage(const std::string &json)
{
    return Message::fromJson(nlohmann::json::parse(json));
}

static MessageComponent parseComponent(const std::string &json)
{
    return MessageComponent::fromJson(nlohmann::json::parse(json));
}

TEST_CASE("Action row parses legacy button variants", "[components]")
{
    const std::string json = R"({"type":1,"id":0,"components":[)"
        R"({"type":2,"id":1,"style":1,"label":"Click","custom_id":"btn_click","disabled":false},)"
        R"({"type":2,"id":2,"style":5,"label":"Docs","url":"https://example.com"},)"
        R"({"type":2,"id":3,"style":6,"sku_id":"123456789012345678"},)"
        R"({"type":2,"id":4,"style":2,"custom_id":"btn_party","emoji":{"name":"🎉","id":null},"disabled":false})"
        R"(]})";

    const MessageComponent row = parseComponent(json);
    REQUIRE(row.type.get() == MessageComponentType::ACTION_ROW);
    REQUIRE(row.id.hasValue());
    REQUIRE(row.id.get() == 0);
    REQUIRE(row.components.hasValue());

    const auto &buttons = row.components.get();
    REQUIRE(buttons.size() == 4);

    REQUIRE(buttons[0].type.get() == MessageComponentType::BUTTON);
    REQUIRE(buttons[0].style.valueOr(0) == 1);
    REQUIRE(buttons[0].label.valueOr("") == "Click");
    REQUIRE(buttons[0].customId.valueOr("") == "btn_click");
    REQUIRE(buttons[0].disabled.hasValue());
    REQUIRE_FALSE(buttons[0].disabled.get());

    REQUIRE(buttons[1].type.get() == MessageComponentType::BUTTON);
    REQUIRE(buttons[1].style.valueOr(0) == 5);
    REQUIRE(buttons[1].url.valueOr("") == "https://example.com");

    REQUIRE(buttons[2].type.get() == MessageComponentType::BUTTON);
    REQUIRE(buttons[2].skuId.hasValue());
    REQUIRE(buttons[2].skuId.get() == Snowflake(123456789012345678ULL));

    REQUIRE(buttons[3].type.get() == MessageComponentType::BUTTON);
    REQUIRE(buttons[3].emoji.hasValue());
    REQUIRE(buttons[3].emoji->name.valueOr("") == "🎉");
    REQUIRE_FALSE(buttons[3].disabled.get());
}

TEST_CASE("Select components wrap each in dedicated action row", "[components]")
{
    const std::string json = R"({"id":"1000","channel_id":"2000","components":[)"
        R"({"type":1,"id":10,"components":[{"type":3,"id":11,"custom_id":"select_string","placeholder":"Pick","min_values":0,"max_values":2,"disabled":false,"options":[{"label":"A","value":"a","description":"Opt A","default":true},{"label":"B","value":"b","emoji":{"name":"⭐","id":null},"default":false}]}]},)"
        R"({"type":1,"id":20,"components":[{"type":5,"id":21,"custom_id":"select_user","default_values":[{"id":"123456789012345678","type":"user"}]}]},)"
        R"({"type":1,"id":30,"components":[{"type":6,"id":31,"custom_id":"select_role","default_values":[{"id":"999888777666555","type":"role"}]}]},)"
        R"({"type":1,"id":40,"components":[{"type":7,"id":41,"custom_id":"select_mentionable","default_values":[{"id":"111222333444555","type":"user"}]}]},)"
        R"({"type":1,"id":50,"components":[{"type":8,"id":51,"custom_id":"select_channel","channel_types":[0,2],"default_values":[{"id":"444555666777888","type":"channel"}]}]})"
        R"(]})";

    const Message message = parseMessage(json);
    REQUIRE(message.components.hasValue());
    REQUIRE(message.components->size() == 5);

    // STRING_SELECT
    const auto &strSelect = message.components.get()[0].components.get()[0];
    REQUIRE(strSelect.type.get() == MessageComponentType::STRING_SELECT);
    REQUIRE(strSelect.customId.valueOr("") == "select_string");
    REQUIRE(strSelect.minValues.hasValue());
    REQUIRE(strSelect.minValues.get() == 0);
    REQUIRE(strSelect.options.hasValue());
    REQUIRE(strSelect.options->size() == 2);
    REQUIRE(strSelect.options.get()[0].isDefault.hasValue());
    REQUIRE(strSelect.options.get()[0].isDefault.get());
    REQUIRE(strSelect.options.get()[1].isDefault.hasValue());
    REQUIRE_FALSE(strSelect.options.get()[1].isDefault.get());
    REQUIRE(strSelect.options.get()[1].emoji.hasValue());
    REQUIRE(strSelect.options.get()[1].emoji->name.valueOr("") == "⭐");

    // USER_SELECT
    const auto &userSelect = message.components.get()[1].components.get()[0];
    REQUIRE(userSelect.type.get() == MessageComponentType::USER_SELECT);
    REQUIRE(userSelect.defaultValues.hasValue());
    REQUIRE(userSelect.defaultValues->front().id.get() == Snowflake(123456789012345678ULL));

    // ROLE_SELECT
    const auto &roleSelect = message.components.get()[2].components.get()[0];
    REQUIRE(roleSelect.type.get() == MessageComponentType::ROLE_SELECT);
    REQUIRE(roleSelect.defaultValues->front().id.get() == Snowflake(999888777666555ULL));

    // MENTIONABLE_SELECT
    const auto &mentionSelect = message.components.get()[3].components.get()[0];
    REQUIRE(mentionSelect.type.get() == MessageComponentType::MENTIONABLE_SELECT);
    REQUIRE(mentionSelect.customId.valueOr("") == "select_mentionable");
    REQUIRE(mentionSelect.defaultValues->front().id.get() == Snowflake(111222333444555ULL));

    // CHANNEL_SELECT
    const auto &channelSelect = message.components.get()[4].components.get()[0];
    REQUIRE(channelSelect.type.get() == MessageComponentType::CHANNEL_SELECT);
    REQUIRE(channelSelect.channelTypes.hasValue());
    const std::vector<ChannelType> expectedTypes = { ChannelType::GUILD_TEXT, ChannelType::GUILD_VOICE };
    REQUIRE(channelSelect.channelTypes.get() == expectedTypes);
    REQUIRE(channelSelect.defaultValues->front().id.get() == Snowflake(444555666777888ULL));
}

TEST_CASE("Container parses nested section with thumbnail accessory, gallery and separator", "[components]")
{
    const std::string json = R"({"type":17,"id":20,"accent_color":3447003,"spoiler":false,"components":[)"
        R"({"type":9,"id":21,"components":[{"type":10,"id":22,"content":"Section text"}],"accessory":{"type":11,"id":23,"description":"Thumb preview","spoiler":false,"media":{"url":"https://cdn.example.com/thumb.png","width":80,"height":80}}},)"
        R"({"type":10,"id":24,"content":"Body text"},)"
        R"({"type":12,"id":25,"items":[{"media":{"url":"https://cdn.example.com/art.jpg","proxy_url":"https://media.example.com/art.jpg","width":1920,"height":1080},"description":"Art caption","spoiler":false}]},)"
        R"({"type":14,"id":26,"divider":true,"spacing":1})"
        R"(]})";

    const MessageComponent container = parseComponent(json);
    REQUIRE(container.type.get() == MessageComponentType::CONTAINER);
    REQUIRE(container.accentColor.hasValue());
    REQUIRE(container.accentColor.get() == 3447003U);
    REQUIRE(container.components.hasValue());

    const auto &children = container.components.get();
    REQUIRE(children.size() == 4);

    // Section with THUMBNAIL accessory
    REQUIRE(children[0].type.get() == MessageComponentType::SECTION);
    REQUIRE(children[0].accessory != nullptr);
    REQUIRE(children[0].accessory->type.get() == MessageComponentType::THUMBNAIL);
    REQUIRE(children[0].accessory->description.valueOr("") == "Thumb preview");
    REQUIRE(children[0].accessory->media.hasValue());
    REQUIRE(children[0].accessory->media->url.get() == "https://cdn.example.com/thumb.png");
    REQUIRE(children[0].accessory->media->width.valueOr(0) == 80);

    // Gallery item
    REQUIRE(children[2].type.get() == MessageComponentType::MEDIA_GALLERY);
    REQUIRE(children[2].items.hasValue());
    const auto &item = children[2].items->front();
    REQUIRE(item.description.valueOr("") == "Art caption");
    REQUIRE(item.media.get().proxyUrl.valueOr("") == "https://media.example.com/art.jpg");
    REQUIRE(item.media.get().width.valueOr(0) == 1920);

    // Separator
    REQUIRE(children[3].type.get() == MessageComponentType::SEPARATOR);
    REQUIRE(children[3].divider.hasValue());
    REQUIRE(children[3].divider.get());
    REQUIRE(children[3].spacing.valueOr(0) == 1);
}

TEST_CASE("File component parses attachment scheme URL, filename and byte size", "[components]")
{
    const std::string json = R"({"type":13,"id":30,"name":"spec.pdf","size":1048576,"spoiler":true,"file":{"url":"attachment://spec.pdf","proxy_url":"https://media.example.com/spec.pdf"}})";

    const MessageComponent fileComp = parseComponent(json);
    REQUIRE(fileComp.type.get() == MessageComponentType::FILE);
    REQUIRE(fileComp.name.valueOr("") == "spec.pdf");
    REQUIRE(fileComp.size.valueOr(0) == 1048576);
    REQUIRE(fileComp.spoiler.hasValue());
    REQUIRE(fileComp.spoiler.get());
    REQUIRE(fileComp.file.hasValue());
    REQUIRE(fileComp.file->url.get() == "attachment://spec.pdf");
    REQUIRE(fileComp.file->proxyUrl.valueOr("") == "https://media.example.com/spec.pdf");
}

TEST_CASE("Nullable fields distinguish explicit null from omitted undefined", "[components]")
{
    const std::string nullJson = R"({"type":17,"id":40,"accent_color":null,"components":[{"type":12,"id":41,"items":[{"media":{"url":"https://cdn.example.com/p.png","width":null,"height":null},"description":null}]}]})";
    const MessageComponent nullComp = parseComponent(nullJson);
    REQUIRE(nullComp.accentColor.isNull());
    REQUIRE_FALSE(nullComp.accentColor.isUndefined());
    const auto &nullItem = nullComp.components->front().items->front();
    REQUIRE(nullItem.description.isNull());
    REQUIRE_FALSE(nullItem.description.isUndefined());
    REQUIRE(nullItem.media.get().width.isNull());
    REQUIRE_FALSE(nullItem.media.get().width.isUndefined());

    const std::string omittedJson = R"({"type":17,"id":42,"components":[{"type":12,"id":43,"items":[{"media":{"url":"https://cdn.example.com/p.png"}}]}]})";
    const MessageComponent omittedComp = parseComponent(omittedJson);
    REQUIRE(omittedComp.accentColor.isUndefined());
    REQUIRE_FALSE(omittedComp.accentColor.isNull());
    const auto &omittedItem = omittedComp.components->front().items->front();
    REQUIRE(omittedItem.description.isUndefined());
    REQUIRE_FALSE(omittedItem.description.isNull());
    REQUIRE(omittedItem.media.get().width.isUndefined());
    REQUIRE_FALSE(omittedItem.media.get().width.isNull());
}

TEST_CASE("Zero and false fields are preserved on component boundaries", "[components]")
{
    const std::string json = R"({"type":17,"id":0,"accent_color":0,"components":[)"
        R"({"type":14,"id":0,"divider":false,"spacing":1})"
        R"(]})";

    const MessageComponent container = parseComponent(json);
    REQUIRE(container.id.hasValue());
    REQUIRE(container.id.get() == 0);
    REQUIRE(container.accentColor.hasValue());
    REQUIRE(container.accentColor.get() == 0U);
    REQUIRE_FALSE(container.accentColor.isNull());

    const auto &children = container.components.get();
    REQUIRE(children[0].divider.hasValue());
    REQUIRE_FALSE(children[0].divider.get());
}

TEST_CASE("Component IDs preserve uint32 max alongside full-width snowflakes", "[components]")
{
    const std::string json = R"({"id":"1000","channel_id":"2000","components":[)"
        R"({"type":1,"id":100,"components":[{"type":2,"id":4294967295,"style":6,"sku_id":"18446744073709551614"}]},)"
        R"({"type":1,"id":101,"components":[{"type":5,"id":4294967294,"custom_id":"max_select","default_values":[{"id":"18446744073709551615","type":"user"}]}]})"
        R"(]})";

    const Message message = parseMessage(json);
    const auto &btn = message.components.get()[0].components.get()[0];
    REQUIRE(btn.id.hasValue());
    REQUIRE(btn.id.get() == 4294967295U);
    REQUIRE(btn.skuId.hasValue());
    REQUIRE(btn.skuId.get() == Snowflake(18446744073709551614ULL));

    const auto &select = message.components.get()[1].components.get()[0];
    REQUIRE(select.id.hasValue());
    REQUIRE(select.id.get() == 4294967294U);
    REQUIRE(select.defaultValues->front().id.get() == Snowflake(18446744073709551615ULL));
}

TEST_CASE("Unknown and modal types ignore unrelated fields and retain siblings", "[components]")
{
    const std::string json = R"({"type":1,"id":500,"components":[)"
        R"({"type":99,"id":501,"label":{"unexpected":"object"},"custom_id":"ignored"},)"
        R"({"type":4,"id":502,"custom_id":"modal_text_input"},)"
        R"({"type":2,"id":503,"style":1,"custom_id":"known_btn","label":"Known Button"})"
        R"(]})";

    const MessageComponent row = parseComponent(json);
    const auto &items = row.components.get();
    REQUIRE(items.size() == 3);

    REQUIRE(items[0].type.get() == static_cast<MessageComponentType>(99));
    REQUIRE(items[0].id.get() == 501);
    REQUIRE(items[0].label.isUndefined());
    REQUIRE(items[0].customId.isUndefined());

    REQUIRE(items[1].type.get() == static_cast<MessageComponentType>(4));
    REQUIRE(items[1].id.get() == 502);
    REQUIRE(items[1].customId.isUndefined());

    REQUIRE(items[2].type.get() == MessageComponentType::BUTTON);
    REQUIRE(items[2].id.get() == 503);
    REQUIRE(items[2].customId.valueOr("") == "known_btn");
    REQUIRE(items[2].label.valueOr("") == "Known Button");
}

TEST_CASE("Message updates preserve on omit, replace with exact raw JSON and clear on empty", "[components]")
{
    const std::string initialJson = R"({"id":"1","channel_id":"2","components":[{"type":1,"id":10,"components":[{"type":2,"id":11,"label":"Save","style":1,"custom_id":"btn_save"}]}]})";
    Message message = parseMessage(initialJson);
    REQUIRE(message.components.hasValue());
    const std::string initialRaw = message.componentsJson;

    // Omitted components leaves previous parsed values and raw JSON unchanged
    message.applyUpdate(parseMessage(R"({"id":"1","channel_id":"2","content":"Omitted update"})"));
    REQUIRE(message.components.hasValue());
    REQUIRE(message.components->front().components->front().id.get() == 11);
    REQUIRE(message.components->front().components->front().label.valueOr("") == "Save");
    REQUIRE(message.componentsJson == initialRaw);

    // Replacement updates parsed components and exact raw JSON
    const std::string replRaw = R"([{"divider":true,"id":12,"spacing":1,"type":14}])";
    message.applyUpdate(parseMessage(std::string(R"({"id":"1","channel_id":"2","components":)") + replRaw + "}"));
    REQUIRE(message.components.hasValue());
    REQUIRE(message.components->size() == 1);
    REQUIRE(message.components->front().type.get() == MessageComponentType::SEPARATOR);
    REQUIRE(message.components->front().id.get() == 12);
    REQUIRE(message.componentsJson == replRaw);

    // Explicit empty array clears components and yields "[]"
    message.applyUpdate(parseMessage(R"({"id":"1","channel_id":"2","components":[]})"));
    REQUIRE(message.components.hasValue());
    REQUIRE(message.components->empty());
    REQUIRE(message.componentsJson == "[]");
}

TEST_CASE("Referenced messages retain nested components and IS_COMPONENTS_V2 flag", "[components]")
{
    const std::string json = R"({"id":"100","channel_id":"200","content":"","flags":32768,)"
        R"("components":[{"type":17,"id":10,"components":[{"type":10,"id":11,"content":"Outer text"}]}],)"
        R"("referenced_message":{"id":"101","channel_id":"200","content":"","flags":32768,)"
        R"("components":[{"type":1,"id":20,"components":[{"type":2,"id":21,"style":1,"custom_id":"reply_btn","label":"Reply"}]}]}})";

    const Message message = parseMessage(json);
    REQUIRE(message.flags.get().testFlag(MessageFlag::IS_COMPONENTS_V2));
    REQUIRE(message.components.hasValue());
    REQUIRE(message.components->front().type.get() == MessageComponentType::CONTAINER);

    REQUIRE(message.referencedMessage != nullptr);
    REQUIRE(message.referencedMessage->flags.get().testFlag(MessageFlag::IS_COMPONENTS_V2));
    REQUIRE(message.referencedMessage->components.hasValue());
    REQUIRE(message.referencedMessage->components->front().components->front().customId.valueOr("") == "reply_btn");
}

TEST_CASE("Forwarded message snapshots retain nested components and IS_COMPONENTS_V2 flag", "[components]")
{
    const std::string json = R"({"id":"200","channel_id":"300","content":"","flags":32768,)"
        R"("message_reference":{"type":1,"channel_id":"300","message_id":"201"},)"
        R"("message_snapshots":[{"message":{"id":"201","channel_id":"300","content":"","flags":32768,)"
        R"("components":[{"type":17,"id":30,"components":[{"type":10,"id":31,"content":"Snapshot text"}]}]}}]})";

    const Message message = parseMessage(json);
    REQUIRE(message.isForwarded());
    REQUIRE(message.snapshotMessage != nullptr);
    REQUIRE(message.snapshotMessage->flags.get().testFlag(MessageFlag::IS_COMPONENTS_V2));
    REQUIRE(message.contentMessage().components.hasValue());
    REQUIRE(message.contentMessage().components->front().type.get() == MessageComponentType::CONTAINER);
    REQUIRE(message.contentMessage().components->front().components->front().content.valueOr("") == "Snapshot text");
}
