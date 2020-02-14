#include "util/eventlogs/EventLogs.h"
#include "common/StringUtils.h"
#include "hunt/reaction/Detections.h"
#include "util/log/Log.h"

const int SIZE_DATA = 4096;
const int ARRAY_SIZE = 10;

namespace EventLogs {

	/**
	 * The callback function directly called by event subscriptions.
	 * In turn it calls the EventSubscription::SubscriptionCallback of a specific class instance.
	 */
	DWORD WINAPI CallbackWrapper(EVT_SUBSCRIBE_NOTIFY_ACTION Action, PVOID UserContext, EVT_HANDLE Event) {
		return reinterpret_cast<EventSubscription*>(UserContext)->SubscriptionCallback(Action, Event);
	}

	std::optional<std::wstring> EventLogs::GetEventParam(const EventWrapper& hEvent, const std::wstring& param) {
		auto queryParam = param.c_str();
		EventWrapper hContext = EvtCreateRenderContext(1, &queryParam, EvtRenderContextValues);
		if (!hContext){
			LOG_ERROR("EventLogs::GetEventParam: EvtCreateRenderContext failed with " + std::to_string(GetLastError()));
			return std::nullopt;
		}

		DWORD dwBufferSize{};
		if(!EvtRender(hContext, hEvent, EvtRenderEventValues, dwBufferSize, nullptr, &dwBufferSize, nullptr)){
			if(ERROR_INSUFFICIENT_BUFFER == GetLastError()){
				auto pRenderedValues = AllocationWrapper{ HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwBufferSize), dwBufferSize, AllocationWrapper::HEAP_ALLOC };
				if(pRenderedValues){
					if(EvtRender(hContext, hEvent, EvtRenderEventValues, dwBufferSize, pRenderedValues, &dwBufferSize, nullptr)){
						PEVT_VARIANT result = reinterpret_cast<PEVT_VARIANT>((LPVOID) pRenderedValues);
						if(result->Type == EvtVarTypeString)
							return std::wstring(result->StringVal);
						else if(result->Type == EvtVarTypeFileTime) {
							wchar_t ar[30];
							_ui64tow(result->FileTimeVal, ar, 10);
							return ar;
						} else if(result->Type == EvtVarTypeUInt16) {
							return std::to_wstring(result->UInt16Val);
						} else if(result->Type == EvtVarTypeUInt64) {
							return std::to_wstring(result->UInt64Val);
						} else if(result->Type == EvtVarTypeNull)
							return L"NULL";
						else {
							return L"Unknown VARIANT: " + std::to_wstring(result->Type);
						}
					}
				}
			}
		}
		return std::nullopt;
	}

	std::optional<std::wstring> EventLogs::GetEventXML(const EventWrapper& hEvent){
		DWORD dwBufferSize = 0;
		if(!EvtRender(NULL, hEvent, EvtRenderEventXml, dwBufferSize, nullptr, &dwBufferSize, nullptr)){
			if (ERROR_INSUFFICIENT_BUFFER == GetLastError()){
				auto pRenderedContent = AllocationWrapper{ HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwBufferSize), dwBufferSize, AllocationWrapper::HEAP_ALLOC };
				if (pRenderedContent){
					if(EvtRender(NULL, hEvent, EvtRenderEventXml, dwBufferSize, pRenderedContent, &dwBufferSize, nullptr)){
						return reinterpret_cast<LPCWSTR>((LPVOID)pRenderedContent);
					}
				}
			}
		}

		return std::nullopt;
	}

	// Enumerate all the events in the result set. 
	std::vector<EventLogItem> EventLogs::ProcessResults(const EventWrapper& hResults, const std::vector<XpathQuery>& filters) {
		EVT_HANDLE hEvents[ARRAY_SIZE];

		std::vector<EventLogItem> results;
		std::vector<std::wstring> params;
		for(auto query : filters){
			if(!query.SearchesByValue()){
				params.push_back(query.ToString());
			}
		}

		DWORD dwReturned{};
		while(EvtNext(hResults, ARRAY_SIZE, hEvents, INFINITE, 0, &dwReturned)){
			for(DWORD i = 0; i < dwReturned; i++) {

				auto item = EventToEventLogItem(hEvents[i], params);
				if(item){
					results.push_back(*item);
				}

				EvtClose(hEvents[i]);
				hEvents[i] = NULL;
			}
		}

		for(unsigned i = 0; i < ARRAY_SIZE; i++){
			if(hEvents[i]){
				EvtClose(hEvents[i]);
			}
		}

		if(GetLastError() != ERROR_NO_MORE_ITEMS){
			LOG_ERROR("EventLogs::ProcessResults: EvtNext failed with " << GetLastError());
		}

		return results;
	}

	std::optional<EventLogItem> EventToEventLogItem(const EventWrapper& hEvent, const std::vector<std::wstring>& params){
		DWORD status = ERROR_SUCCESS;

		std::optional<std::wstring> eventIDStr, eventRecordIDStr, timeCreated, channel, rawXML;

		if (std::nullopt == (eventIDStr = GetEventParam(hEvent, L"Event/System/EventID")))
			return std::nullopt;
		if (std::nullopt == (eventRecordIDStr = GetEventParam(hEvent, L"Event/System/EventRecordID")))
			return std::nullopt;
		if (std::nullopt == (timeCreated = GetEventParam(hEvent, L"Event/System/TimeCreated/@SystemTime")))
			return std::nullopt;
		if (std::nullopt == (channel = GetEventParam(hEvent, L"Event/System/Channel")))
			return std::nullopt;
		if (std::nullopt == (rawXML = GetEventXML(hEvent)))
			return std::nullopt;

		EventLogItem pItem{};

		// Provide values for filtered parameters
		for (std::wstring key : params) {
			std::optional<std::wstring> val = GetEventParam(hEvent, key);
			if (!val) {
				return std::nullopt;
			}

			pItem.SetProperty(key, *val);
		}

		pItem.SetEventID(std::stoul(*eventIDStr));
		pItem.SetEventRecordID(std::stoul(*eventRecordIDStr));
		pItem.SetTimeCreated(*timeCreated);
		pItem.SetChannel(*channel);
		pItem.SetXML(*rawXML);

		return pItem;
	}

	std::vector<EventLogItem> EventLogs::QueryEvents(const std::wstring& channel, unsigned int id, const std::vector<XpathQuery>& filters) {

		std::vector<EventLogItem> items;

		auto query = std::wstring(L"Event/System[EventID=") + std::to_wstring(id) + std::wstring(L"]");
		for (auto param : filters)
			query += L" and " + param.ToString();

		EventWrapper hResults = EvtQuery(NULL, channel.c_str(), query.c_str(), EvtQueryChannelPath | EvtQueryReverseDirection);
		if (NULL == hResults) {
			if (ERROR_EVT_CHANNEL_NOT_FOUND == GetLastError())
				LOG_ERROR("EventLogs::QueryEvents: The channel was not found.");
			else if (ERROR_EVT_INVALID_QUERY == GetLastError())
				LOG_ERROR(L"EventLogs::QueryEvents: The query " << query << L" is not valid.");
			else
				LOG_ERROR("EventLogs::QueryEvents: EvtQuery failed with " << GetLastError());
		}
		else {
			items = ProcessResults(hResults, filters);
		}

		return items;
	}

	std::vector<EventSubscription> subscriptions = {};

	std::optional<std::reference_wrapper<EventSubscription>> EventLogs::SubscribeToEvent(const std::wstring& pwsPath, 
		unsigned int id, const std::function<void(EventLogItem)>& callback, const std::vector<XpathQuery>& filters){
		auto query = std::wstring(L"Event/System[EventID=") + std::to_wstring(id) + std::wstring(L"]");
		for (auto param : filters)
			query += L" and " + param.ToString();

		subscriptions.emplace_back(EventSubscription{ callback });
		auto& eventSub = subscriptions[subscriptions.size() - 1];

		EventWrapper hSubscription = EvtSubscribe(NULL, NULL, pwsPath.c_str(), query.c_str(), NULL, &subscriptions[subscriptions.size() - 1], 
			CallbackWrapper, EvtSubscribeToFutureEvents);
		eventSub.setSubHandle(hSubscription);

		if(!hSubscription){
			if (ERROR_EVT_CHANNEL_NOT_FOUND == GetLastError())
				LOG_ERROR("EventLogs::SubscribeToEvent: Channel was not found.");
			else if (ERROR_EVT_INVALID_QUERY == GetLastError())
				LOG_ERROR(L"EventLogs::SubscribeToEvent: query " << query << L" is not valid.");
			else
				LOG_ERROR("EventLogs::SubscribeToEvent: EvtSubscribe failed with " << GetLastError());

			return std::nullopt;
		}

		return eventSub;
	}

	std::shared_ptr<EVENT_DETECTION> EventLogs::EventLogItemToDetection(const EventLogItem& pItem) {
		auto detect = std::make_shared<EVENT_DETECTION>(0, 0, L"", L"", L"");

		detect->eventID = pItem.GetEventID();
		detect->channel = pItem.GetChannel();
		detect->eventRecordID = pItem.GetEventRecordID();
		detect->timeCreated = pItem.GetTimeCreated();
		detect->rawXML = pItem.GetXML();
		detect->params = pItem.GetProperties();

		return detect;
	}

}