#include "PollCreateForm.hpp"

#include "ComponentRenderer.hpp"
#include "Platform/Native.hpp"
#include "Platform/Utf8.hpp"

#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include <algorithm>
#include <charconv>
#include <format>
#include <utility>

namespace Styx::UI::Rml {

namespace {

void setControlDisabled(::Rml::Element *element, bool disabled)
{
    if (!element)
        return;
    if (disabled)
        element->SetAttribute("disabled", "");
    else
        element->RemoveAttribute("disabled");
    element->SetPseudoClass("disabled", disabled);
}

std::optional<std::size_t> resolveIndex(::Rml::Element *element)
{
    for (auto *cur = element; cur; cur = cur->GetParentNode()) {
        if (cur->HasAttribute("data-index")) {
            const std::string s = cur->GetAttribute<std::string>("data-index", "");
            unsigned int idx = 0;
            auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), idx);
            if (ec == std::errc{} && p == s.data() + s.size())
                return static_cast<std::size_t>(idx);
        }
    }
    return std::nullopt;
}

::Rml::Element *findActionElement(::Rml::Element *element)
{
    for (auto *cur = element; cur; cur = cur->GetParentNode()) {
        if (cur->HasAttribute("data-action"))
            return cur;
    }
    return nullptr;
}

bool sameEmoji(const std::optional<Discord::Emoji> &a, const std::optional<Discord::Emoji> &b)
{
    if (!a.has_value() && !b.has_value())
        return true;
    if (a.has_value() != b.has_value())
        return false;
    const bool aCustom = a->id.hasValue() && a->id.get().isValid() && a->id.get() != Core::Snowflake(0);
    const bool bCustom = b->id.hasValue() && b->id.get().isValid() && b->id.get() != Core::Snowflake(0);
    if (aCustom != bCustom)
        return false;
    if (aCustom)
        return a->id.get() == b->id.get();
    return a->name.get() == b->name.get();
}

} // namespace

PollCreateForm::PollCreateForm(ImageSource imageSource, std::function<void()> repaint)
    : m_imageSource(std::move(imageSource))
    , m_repaint(std::move(repaint))
{
    reset();
}

PollCreateForm::~PollCreateForm()
{
    setDocument(nullptr);
}

void PollCreateForm::setDocument(::Rml::ElementDocument *document)
{
    if (doc == document)
        return;

    if (doc) {
        doc->RemoveEventListener("click", this);
        doc->RemoveEventListener("change", this);
        doc->RemoveEventListener("keydown", this);
    }

    doc = document;

    if (doc) {
        doc->AddEventListener("click", this);
        doc->AddEventListener("change", this);
        doc->AddEventListener("keydown", this);

        populateControls();
    }
}

void PollCreateForm::reset()
{
    m_draft.question.clear();
    m_draft.answers.clear();
    m_draft.answers.resize(2);
    m_draft.durationHours = 24;
    m_draft.allowMultiselect = false;
    m_error.clear();
    m_pending = false;

    populateControls();
}

const Discord::PollCreateRequest &PollCreateForm::draft() const
{
    return m_draft;
}

void PollCreateForm::setPending(bool pending)
{
    if (m_pending == pending)
        return;
    m_pending = pending;
    if (!doc)
        return;

    if (m_pending)
        doc->Focus();

    updateAllDisabledStates();
    if (m_repaint)
        m_repaint();
}

void PollCreateForm::setError(std::string_view error)
{
    m_error = std::string(error);
    if (!doc)
        return;

    if (auto *errorEl = doc->GetElementById("create-poll-error")) {
        errorEl->SetInnerRML(Platform::htmlEscape(m_error));
        errorEl->SetProperty("display", m_error.empty() ? "none" : "block");
    }
    if (m_repaint)
        m_repaint();
}

void PollCreateForm::setEmoji(std::size_t index, std::optional<Discord::Emoji> emoji)
{
    if (m_pending || index >= m_draft.answers.size() || sameEmoji(m_draft.answers[index].emoji, emoji))
        return;

    m_draft.answers[index].emoji = std::move(emoji);

    if (doc) {
        if (auto *emojiBtn = doc->GetElementById("create-poll-emoji-" + std::to_string(index))) {
            if (m_draft.answers[index].emoji.has_value())
                emojiBtn->SetInnerRML(renderInlineEmoji(*m_draft.answers[index].emoji, m_imageSource));
            else
                emojiBtn->SetInnerRML("<img class=\"create-poll-emoji-icon\" src=\"icon:smile\" alt=\"Add emoji\"/>");
        }
        ::Rml::ElementList clearBtns;
        doc->QuerySelectorAll(clearBtns, ".create-poll-clear-emoji");
        for (auto *btn : clearBtns) {
            auto idxOpt = resolveIndex(btn);
            if (idxOpt && *idxOpt == index) {
                const bool disabled = m_pending || !m_draft.answers[index].emoji.has_value();
                setControlDisabled(btn, disabled);
                break;
            }
        }
        if (m_repaint)
            m_repaint();
    }

    draftChanged();
}

void PollCreateForm::focusQuestion()
{
    if (doc) {
        if (m_pending)
            doc->Focus();
        else if (auto *q = doc->GetElementById("create-poll-question"))
            q->Focus();
    }
}

void PollCreateForm::focusEmoji(std::size_t index)
{
    if (auto *e = doc ? doc->GetElementById("create-poll-emoji-" + std::to_string(index)) : nullptr)
        e->Focus();
}

Platform::Rect PollCreateForm::emojiAnchor(std::size_t index) const
{
    Platform::Rect anchor;
    if (auto *e = doc ? doc->GetElementById("create-poll-emoji-" + std::to_string(index)) : nullptr) {
        anchor.x = e->GetAbsoluteLeft();
        anchor.y = e->GetAbsoluteTop();
        anchor.width = e->GetClientWidth();
        anchor.height = e->GetClientHeight();
    }
    return anchor;
}

void PollCreateForm::ProcessEvent(::Rml::Event &event)
{
    const auto &type = event.GetType();

    if (type == "keydown") {
        if (event.GetParameter<int>("key_identifier", 0) != ::Rml::Input::KI_ESCAPE)
            return;
        event.StopPropagation();
        cancelRequested.publish();
        return;
    }

    if (type == "change") {
        if (m_pending)
            return;

        auto *target = event.GetTargetElement();
        if (!target)
            return;

        if (target->GetId() == "create-poll-question") {
            if (auto *control = rmlui_dynamic_cast<::Rml::ElementFormControl *>(target)) {
                std::string val = event.GetParameter<::Rml::String>("value", control->GetValue());
                if (val != m_draft.question) {
                    m_draft.question = std::move(val);
                    draftChanged();
                }
            }
            return;
        }

        if (target->GetId().starts_with("create-poll-answer-") || target->HasAttribute("data-index")) {
            auto idxOpt = resolveIndex(target);
            if (idxOpt && *idxOpt < m_draft.answers.size()) {
                if (auto *control = rmlui_dynamic_cast<::Rml::ElementFormControl *>(target)) {
                    std::string val = event.GetParameter<::Rml::String>("value", control->GetValue());
                    if (val != m_draft.answers[*idxOpt].text) {
                        m_draft.answers[*idxOpt].text = std::move(val);
                        draftChanged();
                    }
                }
            }
            return;
        }

        if (target->GetId() == "create-poll-duration") {
            if (auto *control = rmlui_dynamic_cast<::Rml::ElementFormControl *>(target)) {
                const std::string val = event.GetParameter<::Rml::String>("value", control->GetValue());
                int hours = 0;
                auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), hours);
                const int parsedHours = (ec == std::errc{} && ptr == val.data() + val.size()) ? hours : 0;
                if (m_draft.durationHours != parsedHours) {
                    m_draft.durationHours = parsedHours;
                    draftChanged();
                }
            }
            return;
        }

        if (target->GetId() == "create-poll-multiselect") {
            const bool checked = target->HasAttribute("checked");
            if (m_draft.allowMultiselect != checked) {
                m_draft.allowMultiselect = checked;
                draftChanged();
            }
            return;
        }

        return;
    }

    if (type == "click") {
        auto *actionEl = findActionElement(event.GetTargetElement());
        if (!actionEl)
            return;

        const std::string action = actionEl->GetAttribute<std::string>("data-action", "");
        if (action == "poll-cancel") {
            cancelRequested.publish();
            return;
        }

        if (m_pending || actionEl->HasAttribute("disabled"))
            return;

        if (action == "poll-create") {
            if (!canSubmit())
                return;
            submitRequested.publish();
            return;
        }

        if (action == "poll-add-answer") {
            if (m_draft.answers.size() >= 10)
                return;
            m_draft.answers.push_back({});
            rebuildAnswerRows();
            const std::size_t newIdx = m_draft.answers.size() - 1;
            if (doc) {
                if (auto *input = doc->GetElementById("create-poll-answer-" + std::to_string(newIdx)))
                    input->Focus();
            }
            draftChanged();
            return;
        }

        if (action == "poll-remove-answer") {
            if (m_draft.answers.size() <= 2)
                return;
            auto idxOpt = resolveIndex(actionEl);
            if (!idxOpt || *idxOpt >= m_draft.answers.size())
                return;
            const std::size_t removedIdx = *idxOpt;
            m_draft.answers.erase(m_draft.answers.begin() + removedIdx);
            rebuildAnswerRows();
            const std::size_t focusIdx = (removedIdx < m_draft.answers.size()) ? removedIdx : (m_draft.answers.size() - 1);
            if (doc) {
                if (auto *input = doc->GetElementById("create-poll-answer-" + std::to_string(focusIdx)))
                    input->Focus();
            }
            draftChanged();
            return;
        }

        if (action == "poll-answer-emoji") {
            auto idxOpt = resolveIndex(actionEl);
            if (!idxOpt || *idxOpt >= m_draft.answers.size())
                return;
            emojiRequested.publish(*idxOpt);
            return;
        }

        if (action == "poll-clear-emoji") {
            auto idxOpt = resolveIndex(actionEl);
            if (!idxOpt || *idxOpt >= m_draft.answers.size())
                return;
            setEmoji(*idxOpt, std::nullopt);
            return;
        }
    }
}

void PollCreateForm::rebuildAnswerRows()
{
    if (!doc)
        return;
    auto *answersHost = doc->GetElementById("create-poll-answers");
    if (!answersHost)
        return;

    std::string html;
    for (std::size_t i = 0; i < m_draft.answers.size(); ++i) {
        const auto &ans = m_draft.answers[i];
        std::string emojiHtml;
        if (ans.emoji.has_value())
            emojiHtml = renderInlineEmoji(*ans.emoji, m_imageSource);
        else
            emojiHtml = "<img class=\"create-poll-emoji-icon\" src=\"icon:smile\" alt=\"Add emoji\"/>";

        const bool hasEmoji = ans.emoji.has_value();
        const bool canRemove = m_draft.answers.size() > 2;

        html += std::format(
            "<div class=\"create-poll-answer-row\" data-index=\"{0}\">"
            "<button id=\"create-poll-emoji-{0}\" class=\"create-poll-emoji\" data-action=\"poll-answer-emoji\" data-index=\"{0}\" title=\"Add emoji\"{1}>{2}</button>"
            "<button class=\"create-poll-clear-emoji\" data-action=\"poll-clear-emoji\" data-index=\"{0}\" title=\"Remove emoji\"{3}><img class=\"create-poll-clear-emoji-icon\" src=\"icon:x\" alt=\"Remove emoji\"/></button>"
            "<input type=\"text\" id=\"create-poll-answer-{0}\" class=\"create-poll-answer-input\" maxlength=\"55\" data-index=\"{0}\" placeholder=\"Option {4}\" value=\"{5}\"{6}/>"
            "<button class=\"create-poll-remove-answer\" data-action=\"poll-remove-answer\" data-index=\"{0}\" title=\"Remove option\"{7}><img class=\"create-poll-remove-answer-icon\" src=\"icon:x\" alt=\"Remove option\"/></button>"
            "</div>",
            i,
            m_pending ? " disabled" : "",
            emojiHtml,
            (m_pending || !hasEmoji) ? " disabled" : "",
            i + 1,
            Platform::htmlEscape(ans.text),
            m_pending ? " disabled" : "",
            (m_pending || !canRemove) ? " disabled" : ""
        );
    }
    answersHost->SetInnerRML(html);
    updateAllDisabledStates();
}

void PollCreateForm::updateAllDisabledStates()
{
    if (!doc)
        return;

    if (auto *addBtn = doc->GetElementById("create-poll-add"))
        setControlDisabled(addBtn, m_pending || m_draft.answers.size() >= 10);

    updateSubmitButton();

    if (auto *q = doc->GetElementById("create-poll-question"))
        setControlDisabled(q, m_pending);

    if (auto *d = doc->GetElementById("create-poll-duration"))
        setControlDisabled(d, m_pending);

    if (auto *m = doc->GetElementById("create-poll-multiselect"))
        setControlDisabled(m, m_pending);

    const bool canRemove = !m_pending && m_draft.answers.size() > 2;
    for (std::size_t i = 0; i < m_draft.answers.size(); ++i) {
        if (auto *emojiBtn = doc->GetElementById("create-poll-emoji-" + std::to_string(i)))
            setControlDisabled(emojiBtn, m_pending);
        if (auto *input = doc->GetElementById("create-poll-answer-" + std::to_string(i)))
            setControlDisabled(input, m_pending);
    }

    ::Rml::ElementList clearBtns;
    doc->QuerySelectorAll(clearBtns, ".create-poll-clear-emoji");
    for (auto *btn : clearBtns) {
        auto idxOpt = resolveIndex(btn);
        const bool disabled = m_pending || !idxOpt || *idxOpt >= m_draft.answers.size() || !m_draft.answers[*idxOpt].emoji.has_value();
        setControlDisabled(btn, disabled);
    }

    ::Rml::ElementList removeBtns;
    doc->QuerySelectorAll(removeBtns, ".create-poll-remove-answer");
    for (auto *btn : removeBtns)
        setControlDisabled(btn, !canRemove);

    if (auto *statusEl = doc->GetElementById("create-poll-status")) {
        statusEl->SetProperty("display", m_pending ? "block" : "none");
        statusEl->SetInnerRML(m_pending ? "Creating poll…" : "");
    }
}

void PollCreateForm::updateSubmitButton()
{
    if (!doc)
        return;
    if (auto *submitBtn = doc->GetElementById("create-poll-submit")) {
        const bool enabled = !m_pending && canSubmit();
        setControlDisabled(submitBtn, !enabled);
    }
}

void PollCreateForm::populateControls()
{
    if (!doc)
        return;
    if (auto *q = rmlui_dynamic_cast<::Rml::ElementFormControl *>(doc->GetElementById("create-poll-question")))
        q->SetValue(m_draft.question);
    if (auto *d = rmlui_dynamic_cast<::Rml::ElementFormControl *>(doc->GetElementById("create-poll-duration")))
        d->SetValue(std::to_string(m_draft.durationHours));
    if (auto *m = doc->GetElementById("create-poll-multiselect")) {
        if (m_draft.allowMultiselect)
            m->SetAttribute("checked", "");
        else
            m->RemoveAttribute("checked");
    }
    setError(m_error);
    rebuildAnswerRows();
    if (m_repaint)
        m_repaint();
}

void PollCreateForm::draftChanged()
{
    if (!m_error.empty())
        setError({});
    updateSubmitButton();
    changed.publish();
    if (m_repaint)
        m_repaint();
}

bool PollCreateForm::canSubmit() const
{
    if (Platform::trim(m_draft.question).empty())
        return false;
    if (m_draft.answers.size() < 2 || m_draft.answers.size() > 10)
        return false;
    for (const auto &ans : m_draft.answers) {
        if (Platform::trim(ans.text).empty())
            return false;
    }
    return true;
}

} // namespace Styx::UI::Rml
