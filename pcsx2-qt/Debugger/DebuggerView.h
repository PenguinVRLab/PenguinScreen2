// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "QtHost.h"
#include "Debugger/DebuggerEvents.h"

#include "DebugTools/DebugInterface.h"

#include <QtWidgets/QWidget>

class JsonValueWrapper;

struct DebuggerViewParameters
{
	QString unique_name;
	u64 id = 0;
	DebugInterface* cpu = nullptr;
	std::optional<BreakPointCpu> cpu_override;
	QWidget* parent = nullptr;
};

class DebuggerView : public QWidget
{
	Q_OBJECT

public:
	QString uniqueName() const;
	u64 id() const;

	QString displayName() const;
	QString displayNameWithoutSuffix() const;

	QString customDisplayName() const;
	bool setCustomDisplayName(QString display_name);

	bool isPrimary() const;
	void setPrimary(bool is_primary);

	DebugInterface& cpu() const;

	bool setCpu(DebugInterface& new_cpu);

	std::optional<BreakPointCpu> cpuOverride() const;

	bool setCpuOverride(std::optional<BreakPointCpu> new_cpu);

	template <typename Event>
	static void sendEvent(Event event)
	{
		if (!QtHost::IsOnUIThread())
		{
			QtHost::RunOnUIThread([event = std::move(event)]() {
				DebuggerView::sendEventImplementation(event);
			});
			return;
		}

		sendEventImplementation(event);
	}

	template <typename Event>
	static void broadcastEvent(Event event)
	{
		if (!QtHost::IsOnUIThread())
		{
			QtHost::RunOnUIThread([event = std::move(event)]() {
				DebuggerView::broadcastEventImplementation(event);
			});
			return;
		}

		broadcastEventImplementation(event);
	}

	template <typename Event>
	void receiveEvent(std::function<bool(const Event&)> callback)
	{
		m_event_handlers.emplace(
			typeid(Event).name(),
			[callback](const DebuggerEvents::Event& event) -> bool {
				return callback(static_cast<const Event&>(event));
			});
	}

	template <typename Event, typename SubClass>
	void receiveEvent(bool (SubClass::*function)(const Event& event))
	{
		m_event_handlers.emplace(
			typeid(Event).name(),
			[this, function](const DebuggerEvents::Event& event) -> bool {
				return (*static_cast<SubClass*>(this).*function)(static_cast<const Event&>(event));
			});
	}

	bool handleEvent(const DebuggerEvents::Event& event);

	bool acceptsEventType(const char* event_type);

	template <typename Event>
	std::vector<QAction*> createEventActions(
		QMenu* menu,
		std::function<std::optional<Event>()> event_func,
		bool skip_self = true,
		u32 max_top_level_actions = 5)
	{
		return createEventActionsImplementation(
			menu, max_top_level_actions, skip_self, typeid(Event).name(),
			Event::ACTION_STRING, Event::ACTION_OVERFLOW_STRING,
			[event_func]() -> DebuggerEvents::Event* {
				static std::optional<Event> event;
				event = event_func();
				if (!event.has_value())
					return nullptr;

				return static_cast<DebuggerEvents::Event*>(&(*event));
			});
	}

	virtual void toJson(JsonValueWrapper& json);
	virtual bool fromJson(const JsonValueWrapper& json);

	void switchToThisTab();

	bool supportsMultipleInstances();

	void retranslateDisplayName();

	std::optional<int> displayNameSuffixNumber() const;
	void setDisplayNameSuffixNumber(std::optional<int> suffix_number);

	void updateStyleSheet();

	static void goToInDisassembler(u32 address, bool switch_to_tab);
	static void goToInMemoryView(u32 address, bool switch_to_tab);

protected:
	enum Flags
	{
		NO_DEBUGGER_FLAGS = 0,
		DISALLOW_MULTIPLE_INSTANCES = 1 << 0,
		MONOSPACE_FONT = 1 << 1
	};

	DebuggerView(const DebuggerViewParameters& parameters, u32 flags);

private:
	static void sendEventImplementation(const DebuggerEvents::Event& event);
	static void broadcastEventImplementation(const DebuggerEvents::Event& event);

	std::vector<QAction*> createEventActionsImplementation(
		QMenu* menu,
		u32 max_top_level_actions,
		bool skip_self,
		const char* event_type,
		const char* action_string,
		const char* action_overflow_string,
		std::function<const DebuggerEvents::Event*()> event_func);

	u64 m_id;

	QString m_unique_name;

	QString m_custom_display_name;

	QString m_translated_display_name;
	std::optional<int> m_display_name_suffix_number;

	bool m_is_primary = false;

	DebugInterface* m_cpu;
	std::optional<BreakPointCpu> m_cpu_override;
	u32 m_flags;

	std::multimap<std::string, std::function<bool(const DebuggerEvents::Event&)>> m_event_handlers;
};
