#include <TiltedOnlinePCH.h>


#include <World.h>
#include <Events/UpdateEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/CellChangeEvent.h>
#include <Events/ActivateEvent.h>
#include <Events/LockChangeEvent.h>
#include <Events/ScriptAnimationEvent.h>
#include <Services/EnvironmentService.h>
#include <Services/ImguiService.h>
#include <Messages/ServerTimeSettings.h>
#include <Messages/AssignObjectsRequest.h>
#include <Messages/AssignObjectsResponse.h>
#include <Messages/ActivateRequest.h>
#include <Messages/NotifyActivate.h>
#include <Messages/LockChangeRequest.h>
#include <Messages/NotifyLockChange.h>
#include <Messages/ScriptAnimationRequest.h>
#include <Messages/NotifyScriptAnimation.h>

#include <TimeManager.h>
#include <PlayerCharacter.h>
#include <Forms/TESObjectCELL.h>

#include <imgui.h>
#include <inttypes.h>

#define ENVIRONMENT_DEBUG 0

constexpr float kTransitionSpeed = 5.f;

bool EnvironmentService::s_gameClockLocked = false;

bool EnvironmentService::AllowGameTick() noexcept
{
    return !s_gameClockLocked;
}

EnvironmentService::EnvironmentService(World& aWorld, entt::dispatcher& aDispatcher, ImguiService& aImguiService, TransportService& aTransport) 
    : m_world(aWorld)
    , m_transport(aTransport)
{
    m_timeUpdateConnection = aDispatcher.sink<ServerTimeSettings>().connect<&EnvironmentService::OnTimeUpdate>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&EnvironmentService::HandleUpdate>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&EnvironmentService::OnDisconnected>(this);
    m_cellChangeConnection = aDispatcher.sink<CellChangeEvent>().connect<&EnvironmentService::OnCellChange>(this);
    m_onActivateConnection = aDispatcher.sink<ActivateEvent>().connect<&EnvironmentService::OnActivate>(this);
    m_activateConnection = aDispatcher.sink<NotifyActivate>().connect<&EnvironmentService::OnActivateNotify>(this);
    m_lockChangeConnection = aDispatcher.sink<LockChangeEvent>().connect<&EnvironmentService::OnLockChange>(this);
    m_lockChangeNotifyConnection = aDispatcher.sink<NotifyLockChange>().connect<&EnvironmentService::OnLockChangeNotify>(this);
    m_assignObjectConnection = aDispatcher.sink<AssignObjectsResponse>().connect<&EnvironmentService::OnAssignObjectsResponse>(this);
    m_scriptAnimationConnection = aDispatcher.sink<ScriptAnimationEvent>().connect<&EnvironmentService::OnScriptAnimationEvent>(this);
    m_scriptAnimationNotifyConnection = aDispatcher.sink<NotifyScriptAnimation>().connect<&EnvironmentService::OnNotifyScriptAnimation>(this);

#if ENVIRONMENT_DEBUG
    m_drawConnection = aImguiService.OnDraw.connect<&EnvironmentService::OnDraw>(this);
#endif

#if TP_SKYRIM64
    EventDispatcherManager::Get()->activateEvent.RegisterSink(this);
#else
    GetEventDispatcher_TESActivateEvent()->RegisterSink(this);
#endif
}

void EnvironmentService::OnTimeUpdate(const ServerTimeSettings& acMessage) noexcept
{
    // disable the game clock
    m_onlineTime.TimeScale = acMessage.TimeScale;
    m_onlineTime.Time = acMessage.Time;
    ToggleGameClock(false);
}

void EnvironmentService::OnDisconnected(const DisconnectedEvent&) noexcept
{
    // signal a time transition
    m_fadeTimer = 0.f;
    m_switchToOffline = true;
}

void EnvironmentService::OnCellChange(const CellChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    auto* pPlayer = PlayerCharacter::Get();

    uint32_t baseId = 0;
    uint32_t modId = 0;
    if (!m_world.GetModSystem().GetServerModId(pPlayer->parentCell->formID, modId, baseId))
        return;

    auto* pCell = RTTI_CAST(TESForm::GetById(baseId), TESForm, TESObjectCELL);
    if (!pCell)
        return;

    Vector<TESObjectREFR*> objects;
    Vector<FormType> formTypes = {FormType::Container, FormType::Door};
    pCell->GetRefsByFormTypes(objects, formTypes);

    AssignObjectsRequest request;

    for (const auto& object : objects)
    {
        ObjectData objectData;
        objectData.CellId.BaseId = baseId;
        objectData.CellId.ModId = modId;

        uint32_t baseId = 0;
        uint32_t modId = 0;
        if (!m_world.GetModSystem().GetServerModId(object->formID, modId, baseId))
            return;

        objectData.Id.BaseId = baseId;
        objectData.Id.ModId = modId;

        if (auto* pLock = object->GetLock())
        {
            objectData.CurrentLockData.IsLocked = pLock->flags;
            objectData.CurrentLockData.LockLevel = pLock->lockLevel;
        }

        request.Objects.push_back(objectData);
    }

    m_transport.Send(request);
}

void EnvironmentService::OnAssignObjectsResponse(const AssignObjectsResponse& acMessage) noexcept
{
    for (const auto& object : acMessage.Objects)
    {
        const auto cObjectId = World::Get().GetModSystem().GetGameId(object.Id);
        if (cObjectId == 0)
            continue;

        auto* pObject = RTTI_CAST(TESForm::GetById(cObjectId), TESForm, TESObjectREFR);
        if (!pObject)
            continue;

        if (object.CurrentLockData != LockData{})
        {
            auto* pLock = pObject->GetLock();

            if (!pLock)
            {
                pLock = pObject->CreateLock();
                if (!pLock)
                    continue;
            }

            pLock->lockLevel = object.CurrentLockData.LockLevel;
            pLock->SetLock(object.CurrentLockData.IsLocked);
            pObject->LockChange();
        }
    }
}

BSTEventResult EnvironmentService::OnEvent(const TESActivateEvent* acEvent, const EventDispatcher<TESActivateEvent>* aDispatcher)
{
#if ENVIRONMENT_DEBUG
    auto view = m_world.view<InteractiveObjectComponent>();

    const auto itor =
        std::find_if(std::begin(view), std::end(view), [id = acEvent->object->formID, view](entt::entity entity) {
            return view.get<InteractiveObjectComponent>(entity).Id == id;
        });

    if (itor == std::end(view))
    {
        AddObjectComponent(acEvent->object);
    }
#endif

    return BSTEventResult::kOk;
}

void EnvironmentService::AddObjectComponent(TESObjectREFR* apObject) noexcept
{
    auto entity = m_world.create();
    auto& interactiveObjectComponent = m_world.emplace<InteractiveObjectComponent>(entity);
    interactiveObjectComponent.Id = apObject->formID;
}

void EnvironmentService::OnActivate(const ActivateEvent& acEvent) noexcept
{
    if (acEvent.ActivateFlag)
    {
#if TP_FALLOUT4
        acEvent.pObject->Activate(acEvent.pActivator, acEvent.pObjectToGet, acEvent.Count, acEvent.DefaultProcessing, acEvent.FromScript, acEvent.IsLooping);
#elif TP_SKYRIM64
        acEvent.pObject->Activate(acEvent.pActivator, acEvent.Unk1, acEvent.pObjectToGet, acEvent.Count, acEvent.DefaultProcessing);
#endif
    }

    if (!m_transport.IsConnected())
        return;

    if (auto* pLock = acEvent.pObject->GetLock())
    {
        if (pLock->flags & 0xFF)
            return;
    }

    ActivateRequest request;

    if (!m_world.GetModSystem().GetServerModId(acEvent.pObject->formID, request.Id))
        return;

    if (!m_world.GetModSystem().GetServerModId(acEvent.pObject->GetCellId(), request.CellId))
        return;

    auto view = m_world.view<FormIdComponent>();
    const auto pEntity =
        std::find_if(std::begin(view), std::end(view), [id = acEvent.pActivator->formID, view](entt::entity entity) {
            return view.get<FormIdComponent>(entity).Id == id;
        });

    if (pEntity == std::end(view))
        return;

    request.ActivatorId = utils::GetServerId(*pEntity);
    if (request.ActivatorId == 0)
        return;

    m_transport.Send(request);
}

void EnvironmentService::OnActivateNotify(const NotifyActivate& acMessage) noexcept
{
    auto view = m_world.view<FormIdComponent>();
    for (auto entity : view)
    {
        uint32_t serverId = utils::GetServerId(entity);
        if (serverId == 0)
            return;

        if (serverId == acMessage.ActivatorId)
        {
            const auto cObjectId = World::Get().GetModSystem().GetGameId(acMessage.Id);
            if (cObjectId == 0)
            {
                spdlog::error("Failed to retrieve object to activate.");
                return;
            }

            auto* pObject = RTTI_CAST(TESForm::GetById(cObjectId), TESForm, TESObjectREFR);
            if (!pObject)
            {
                spdlog::error("Failed to retrieve object to activate.");
                return;
            }

            auto& formIdComponent = view.get<FormIdComponent>(entity);
            auto* pActor = RTTI_CAST(TESForm::GetById(formIdComponent.Id), TESForm, Actor);
            
            if (pActor)
            {
                // unsure if these flags are the best, but these are passed with the papyrus Activate fn
                // might be an idea to have the client send the flags through NotifyActivate
            #if TP_FALLOUT4
                pObject->Activate(pActor, nullptr, 1, 0, 0, 0);
            #elif TP_SKYRIM64
                pObject->Activate(pActor, 0, nullptr, 1, 0);
            #endif
                return;
            }
        }
    }
}

void EnvironmentService::OnLockChange(const LockChangeEvent& acEvent) noexcept
{
    if (!m_transport.IsConnected())
        return;

    LockChangeRequest request;

    if (!m_world.GetModSystem().GetServerModId(acEvent.pObject->formID, request.Id))
        return;

    if (!m_world.GetModSystem().GetServerModId(acEvent.pObject->GetCellId(), request.CellId))
        return;

    request.IsLocked = acEvent.IsLocked;
    request.LockLevel = acEvent.LockLevel;

    m_transport.Send(request);
}

void EnvironmentService::OnLockChangeNotify(const NotifyLockChange& acMessage) noexcept
{
    const auto cObjectId = World::Get().GetModSystem().GetGameId(acMessage.Id);
    if (cObjectId == 0)
    {
        spdlog::error("Failed to retrieve object id to (un)lock.");
        return;
    }

    auto* pObject = RTTI_CAST(TESForm::GetById(cObjectId), TESForm, TESObjectREFR);
    if (!pObject)
    {
        spdlog::error("Failed to retrieve object to (un)lock.");
        return;
    }

    auto* pLock = pObject->GetLock();

    if (!pLock)
    {
        pLock = pObject->CreateLock();
        if (!pLock)
            return;
    }

    pLock->lockLevel = acMessage.LockLevel;
    pLock->SetLock(acMessage.IsLocked);
    pObject->LockChange();
}

void EnvironmentService::OnScriptAnimationEvent(const ScriptAnimationEvent& acEvent) noexcept
{
    ScriptAnimationRequest request;
    request.FormID = acEvent.FormID;
    request.Animation = acEvent.Animation;
    request.EventName = acEvent.EventName;
    spdlog::info("send script anim req");

    m_transport.Send(request);
}

void EnvironmentService::OnNotifyScriptAnimation(const NotifyScriptAnimation& acMessage) noexcept
{
#if TP_SKYRIM64
    if (acMessage.FormID == 0)
        return;

    auto* pForm = TESForm::GetById(acMessage.FormID);
    auto* pObject = RTTI_CAST(pForm, TESForm, TESObjectREFR);

    if (!pObject)
    {
        spdlog::error("error trying to fetch notify script animation object");
        return;
    }

    BSFixedString animation(acMessage.Animation.c_str());
    BSFixedString eventName(acMessage.EventName.c_str());

    pObject->PlayAnimationAndWait(&animation, &eventName);
#endif
}

float EnvironmentService::TimeInterpolate(const TimeModel& aFrom, TimeModel& aTo) const
{
    const auto t = aTo.Time - aFrom.Time;
    if (t < 0.f)
    {
        const auto v = t + 24.f;
        // interpolate on the time difference, not the time
        const auto x = TiltedPhoques::Lerp(0.f, v, m_fadeTimer / kTransitionSpeed) + aFrom.Time;

        return TiltedPhoques::Mod(x, 24.f);
    }
    
    return TiltedPhoques::Lerp(aFrom.Time, aTo.Time, m_fadeTimer / kTransitionSpeed);
}

void EnvironmentService::ToggleGameClock(bool aEnable)
{
    auto* pGameTime = TimeData::Get();
    if (aEnable)
    {
        pGameTime->GameDay->i = m_offlineTime.Day;
        pGameTime->GameMonth->i = m_offlineTime.Month;
        pGameTime->GameYear->i = m_offlineTime.Year;
        pGameTime->TimeScale->f = m_offlineTime.TimeScale;
        pGameTime->GameDaysPassed->f = (m_offlineTime.Time * (1.f / 24.f)) + m_offlineTime.Day;
        pGameTime->GameHour->f = m_offlineTime.Time;
        m_switchToOffline = false;
    }
    else
    {
        m_offlineTime.Day = pGameTime->GameDay->i;
        m_offlineTime.Month = pGameTime->GameMonth->i;
        m_offlineTime.Year = pGameTime->GameYear->i;
        m_offlineTime.Time = pGameTime->GameHour->f;
        m_offlineTime.TimeScale = pGameTime->TimeScale->f;
    }

    s_gameClockLocked = !aEnable;
}

void EnvironmentService::HandleUpdate(const UpdateEvent& aEvent) noexcept
{
    if (s_gameClockLocked)
    {
        const auto updateDelta = static_cast<float>(aEvent.Delta);
        auto* pGameTime = TimeData::Get();

        if (!m_lastTick)
            m_lastTick = m_world.GetTick();

        const auto now = m_world.GetTick();

        if (m_switchToOffline)
        {
            // time transition out
            if (m_fadeTimer < kTransitionSpeed)
            {
                pGameTime->GameHour->f = TimeInterpolate(m_onlineTime, m_offlineTime);
                // before we quit here we fire this event
                if ((m_fadeTimer + updateDelta) > kTransitionSpeed)
                {
                    m_fadeTimer += updateDelta;
                    ToggleGameClock(true);
                }
                else
                    m_fadeTimer += updateDelta;
            }
        }

        // we got disconnected or the client got ahead of us
        if (now < m_lastTick)
            return;

        const auto delta = now - m_lastTick;
        m_lastTick = now;

        m_onlineTime.Update(delta);
        pGameTime->GameDay->i = m_onlineTime.Day;
        pGameTime->GameMonth->i = m_onlineTime.Month;
        pGameTime->GameYear->i = m_onlineTime.Year;
        pGameTime->TimeScale->f = m_onlineTime.TimeScale;
        pGameTime->GameDaysPassed->f = (m_onlineTime.Time * (1.f / 24.f)) + m_onlineTime.Day;

        // time transition in
        if (m_fadeTimer < kTransitionSpeed)
        {
            pGameTime->GameHour->f = TimeInterpolate(m_offlineTime, m_onlineTime);
            m_fadeTimer += updateDelta;
        }
        else
            pGameTime->GameHour->f = m_onlineTime.Time;
    }
}

void EnvironmentService::OnDraw() noexcept
{
#if ENVIRONMENT_DEBUG
    static uint32_t s_selectedFormId = 0;
    static uint32_t s_selected = 0;
    static uint32_t formId;

    ImGui::SetNextWindowSize(ImVec2(250, 440), ImGuiCond_FirstUseEver);
    ImGui::Begin("Interactive object list");
    ImGui::BeginChild("Objects", ImVec2(0, 200), true);

    const auto view = m_world.view<InteractiveObjectComponent>();
    Vector<entt::entity> entities(view.begin(), view.end());

    int i = 0;
    for (auto entity : entities)
    {
        auto& objectComponent = view.get<InteractiveObjectComponent>(entity);
        auto* pForm = TESForm::GetById(objectComponent.Id);
        auto* pObject = RTTI_CAST(pForm, TESForm, TESObjectREFR);

        if (!pObject)
            continue;

        char name[256];
        sprintf_s(name, std::size(name), "%s (%x)", pObject->baseForm->GetName(), objectComponent.Id);
        if (ImGui::Selectable(name, s_selectedFormId == objectComponent.Id))
            s_selectedFormId = objectComponent.Id;

        if (s_selectedFormId == objectComponent.Id)
            s_selected = i;

        ++i;
    }

    ImGui::EndChild();

    if (s_selected < entities.size())
    {
        auto& objectComponent = view.get<InteractiveObjectComponent>(entities[s_selected]);
        auto* pForm = TESForm::GetById(objectComponent.Id);
        auto* pObject = RTTI_CAST(pForm, TESForm, TESObjectREFR);

        if (pObject)
        {
            uint64_t address = reinterpret_cast<uint64_t>(pObject);
            ImGui::InputScalar("Memory address", ImGuiDataType_U64, &address, 0, 0, "%" PRIx64, ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_CharsHexadecimal);
            if (ImGui::Button("Activate"))
            {
                auto* pActor = PlayerCharacter::Get();
                World::Get().GetRunner().Trigger(ActivateEvent(pObject, pActor, 0, 0, 1, 0, true));
            }
            int formType = pObject->GetFormType();
            ImGui::InputInt("Form type", &formType, 0, 0, ImGuiInputTextFlags_ReadOnly);
            int formTypeBase = pObject->baseForm->GetFormType();
            ImGui::InputInt("Form type base", &formTypeBase, 0, 0, ImGuiInputTextFlags_ReadOnly);
        }
    }

    ImGui::InputScalar("Form ID", ImGuiDataType_U32, &formId, 0, 0, "%" PRIx32, ImGuiInputTextFlags_CharsHexadecimal);
    if (ImGui::Button("Get address from form ID"))
    {
        if (formId)
        {
            auto* pForm = TESForm::GetById(formId);
            auto* pObject = RTTI_CAST(pForm, TESForm, TESObjectREFR);
            if (pObject)
            {
                auto view = m_world.view<InteractiveObjectComponent>();

                const auto itor =
                    std::find_if(std::begin(view), std::end(view), [id = pObject->formID, view](entt::entity entity) {
                        return view.get<InteractiveObjectComponent>(entity).Id == id;
                    });

                if (itor == std::end(view))
                {
                    AddObjectComponent(pObject);
                }
            }
        }
    }

    ImGui::End();
#endif
}
