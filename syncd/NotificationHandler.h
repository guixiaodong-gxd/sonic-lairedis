#pragma once

extern "C" {
#include "laimetadata.h"
}

#include "NotificationQueue.h"
#include "NotificationProcessor.h"
#include "LinecardNotifications.h"

#include "swss/dbconnector.h"
#include "swss/table.h"

#include <string>
#include <vector>
#include <memory>

namespace syncd
{
    class NotificationHandler
    {
    public:

        NotificationHandler(
            _In_ std::shared_ptr<NotificationProcessor> processor);

        virtual ~NotificationHandler();

    public:

        void setLinecardNotifications(
            _In_ const lai_notifications_t& linecardNotifications);

        const lai_notifications_t& getLinecardNotifications() const;

        void updateNotificationsPointers(
            _In_ lai_object_type_t object_type,
            _In_ uint32_t attr_count,
            _In_ lai_attribute_t* attr_list) const;

    public: // members reflecting LAI callbacks

        void onLinecardStateChange(
            _In_ lai_object_id_t linecard_id,
            _In_ lai_oper_status_t linecard_oper_status);

        void onLinecardAlarm(
            _In_ lai_object_id_t linecard_id,
            _In_ lai_alarm_type_t alarm_type,
            _In_ lai_alarm_info_t alarm_info);

        void onApsReportSwitchInfo(
            _In_ lai_object_id_t rid,
            _In_ lai_olp_switch_t switch_info);

    private:

        void generate_linecard_communication_alarm(
            _In_ lai_object_id_t linecard_rid,
            _In_ lai_oper_status_t linecard_oper_status);

        void enqueueNotification(
            _In_ const std::string& op,
            _In_ const std::string& data,
            _In_ const std::vector<swss::FieldValueTuple>& entry);

        void enqueueNotification(
            _In_ const std::string& op,
            _In_ const std::string& data);

    private:

        std::shared_ptr<swss::DBConnector> m_state_db;

        std::unique_ptr<swss::Table> m_linecardtable;

        lai_notifications_t m_notifications;

        std::shared_ptr<NotificationQueue> m_notificationQueue;

        std::shared_ptr<NotificationProcessor> m_processor;
    };
}
