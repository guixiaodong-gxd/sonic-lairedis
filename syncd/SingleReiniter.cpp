#include <thread>
#include <chrono>
#include "SingleReiniter.h"
#include "VidManager.h"
#include "CommandLineOptions.h"
#include "NotificationHandler.h"
#include "RedisClient.h"

#include "swss/logger.h"

#include "meta/lai_serialize.h"

#include <unistd.h>
#include <inttypes.h>

using namespace syncd;
using namespace laimeta;

SingleReiniter::SingleReiniter(
    _In_ std::shared_ptr<RedisClient> client,
    _In_ std::shared_ptr<VirtualOidTranslator> translator,
    _In_ std::shared_ptr<lairedis::LaiInterface> lai,
    _In_ std::shared_ptr<NotificationHandler> handler,
    _In_ const ObjectIdMap& vidToRidMap,
    _In_ const ObjectIdMap& ridToVidMap,
    _In_ const std::vector<std::string>& asicKeys) :
    m_vendorLai(lai),
    m_vidToRidMap(vidToRidMap),
    m_ridToVidMap(ridToVidMap),
    m_asicKeys(asicKeys),
    m_translator(translator),
    m_client(client),
    m_handler(handler)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_DEBUG("%s m_vidToRidMap %d, m_ridToVidMap %d, m_asicKeys %d", __FUNCTION__, (int)m_vidToRidMap.size(), (int)m_ridToVidMap.size(), (int)m_asicKeys.size());

    m_linecard_rid = LAI_NULL_OBJECT_ID;
    m_linecard_vid = LAI_NULL_OBJECT_ID;
}

SingleReiniter::~SingleReiniter()
{
    SWSS_LOG_ENTER();

    // empty
}

std::shared_ptr<LaiLinecard> SingleReiniter::hardReinit()
{
    SWSS_LOG_ENTER();

    SWSS_LOG_TIMER("hard reinit");

    prepareAsicState();

    processLinecards();

    processOids();

    stopPreConfigLinecards();

#ifdef ENABLE_PERF

    double total_create = 0;
    double total_set = 0;

    for (const auto& p : m_perf_create)
    {
        SWSS_LOG_NOTICE("create %s: %d: %f",
            lai_serialize_object_type(p.first).c_str(),
            std::get<0>(p.second),
            std::get<1>(p.second));

        total_create += std::get<1>(p.second);
    }

    for (const auto& p : m_perf_set)
    {
        SWSS_LOG_NOTICE("set %s: %d: %f",
            lai_serialize_object_type(p.first).c_str(),
            std::get<0>(p.second),
            std::get<1>(p.second));

        total_set += std::get<1>(p.second);
    }

    SWSS_LOG_NOTICE("create %lf, set: %lf", total_create, total_set);
#endif

    checkAllIds();

    return m_sw;
}

void SingleReiniter::prepareAsicState()
{
    SWSS_LOG_ENTER();

    SWSS_LOG_TIMER("read asic state m_asicKeys %d", (int)m_asicKeys.size());

    for (auto& key : m_asicKeys)
    {
        lai_object_type_t objectType = getObjectTypeFromAsicKey(key);

        const std::string& strObjectId = getObjectIdFromAsicKey(key);

        auto info = lai_metadata_get_object_type_info(objectType);

        switch (objectType)
        {
        case LAI_OBJECT_TYPE_LINECARD:
            m_linecards[strObjectId] = key;
            m_oids[strObjectId] = key;
            break;

        default:

            if (info->isnonobjectid)
            {
                SWSS_LOG_THROW("passing non object id %s as generic object", info->objecttypename);
            }

            m_oids[strObjectId] = key;
            break;
        }

        m_attributesLists[key] = redisGetAttributesFromAsicKey(key);
    }
}

lai_object_type_t SingleReiniter::getObjectTypeFromAsicKey(
    _In_ const std::string& key)
{
    SWSS_LOG_ENTER();

    auto start = key.find_first_of(":") + 1;
    auto end = key.find(":", start);

    const std::string strObjectType = key.substr(start, end - start);

    lai_object_type_t objectType;
    lai_deserialize_object_type(strObjectType, objectType);

    if (!lai_metadata_is_object_type_valid(objectType))
    {
        SWSS_LOG_THROW("invalid object type: %s on asic key: %s",
            lai_serialize_object_type(objectType).c_str(),
            key.c_str());
    }

    return objectType;
}

std::string SingleReiniter::getObjectIdFromAsicKey(
    _In_ const std::string& key)
{
    SWSS_LOG_ENTER();

    auto start = key.find_first_of(":") + 1;
    auto end = key.find(":", start);

    return key.substr(end + 1);
}

void SingleReiniter::stopPreConfigLinecards()
{
    SWSS_LOG_ENTER();

    if (m_linecards.size() > 1) {
        SWSS_LOG_THROW("multiple linecards %zu in single hard reinit are not allowed", m_linecards.size());
    }
    
    for (const auto& s : m_linecards) {
        std::string strLinecardVid = s.first;
        
        lai_deserialize_object_id(strLinecardVid, m_linecard_vid);
        if (m_linecard_vid == LAI_NULL_OBJECT_ID) {
            SWSS_LOG_THROW("linecard id can't be NULL");
        }
        auto oit = m_oids.find(strLinecardVid);
        if (oit == m_oids.end()) {
            SWSS_LOG_THROW("failed to find VID %s in OIDs map", strLinecardVid.c_str());
        }
        m_linecard_rid = m_vidToRidMap[m_linecard_vid];
        
        lai_attribute_t attr;
        attr.id = LAI_LINECARD_ATTR_STOP_PRE_CONFIGURATION;
        attr.value.booldata = true;
        
        SWSS_LOG_NOTICE("Stop pre-config linecard");
        
        lai_status_t status = m_vendorLai->set(LAI_OBJECT_TYPE_LINECARD, m_linecard_rid, &attr);
        if (status != LAI_STATUS_SUCCESS) {
            SWSS_LOG_THROW("failed to stop pre-config linecard");
        }
    }
}

void SingleReiniter::processLinecards()
{
    SWSS_LOG_ENTER();

    /*
     * If there are any linecards, we need to create them first to perform any
     * other operations.
     *
     * NOTE: This method needs to be revisited if we want to support multiple
     * linecards.
     */

    if (m_linecards.size() > 1)
    {
        SWSS_LOG_THROW("multiple linecards %zu in single hard reinit are not allowed", m_linecards.size());
    }

    /*
     * Sanity check in metadata make sure that there are no mandatory on create
     * and create only attributes that are object id attributes, since we would
     * need create those objects first but we need linecard first. So here we
     * selecting only MANDATORY_ON_CREATE and CREATE_ONLY attributes to create
     * linecard.
     */

    for (const auto& s : m_linecards)
    {
        std::string strLinecardVid = s.first;
        std::string asicKey = s.second;

        lai_deserialize_object_id(strLinecardVid, m_linecard_vid);

        if (m_linecard_vid == LAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_THROW("linecard id can't be NULL");
        }

        auto oit = m_oids.find(strLinecardVid);

        if (oit == m_oids.end())
        {
            SWSS_LOG_THROW("failed to find VID %s in OIDs map", strLinecardVid.c_str());
        }

        std::shared_ptr<LaiAttributeList> list = m_attributesLists[asicKey];

        lai_attribute_t* attrList = list->get_attr_list();

        uint32_t attrCount = list->get_attr_count();

        /*
         * If any of those attributes are pointers, fix them, so they will
         * point to callbacks in syncd memory.
         */

        m_handler->updateNotificationsPointers(LAI_OBJECT_TYPE_LINECARD, attrCount, attrList); // TODO need per linecard template static

        /*
         * Now we need to select only attributes MANDATORY_ON_CREATE and
         * CREATE_ONLY and which will not contain object ids.
         *
         * No need to call processAttributesForOids since we know that there
         * are no OID attributes.
         */

        uint32_t attr_count = 0;            // attr count needed for create
        uint32_t attr_count_left = 0;       // attr count after create

        std::vector<lai_attribute_t> attrs;         // attrs for create
        std::vector<lai_attribute_t> attrs_left;    // attrs for set

        bool is_board_mode_existed = false;
        lai_linecard_board_mode_t board_mode = LAI_LINECARD_BOARD_MODE_L1_400G_CA_100GE;

        for (uint32_t idx = 0; idx < attrCount; ++idx)
        {
            auto meta = lai_metadata_get_attr_metadata(LAI_OBJECT_TYPE_LINECARD, attrList[idx].id);

            if (LAI_HAS_FLAG_MANDATORY_ON_CREATE(meta->flags) || LAI_HAS_FLAG_CREATE_ONLY(meta->flags))
            {
                /*
                 * If attribute is mandatory on create or create only, we need
                 * to select it for linecard create method, since it's required
                 * on create or it will not be possible to change it after
                 * create.
                 *
                 * Currently linecard don't have any conditional attributes but
                 * we could take this into account. Even if any of those
                 * conditional attributes will present, it will be not be oid
                 * attribute.
                 */

                attrs.push_back(attrList[idx]); // struct copy, we will keep the same pointers
                attr_count++;
            }
            else if (attrList[idx].id == LAI_LINECARD_ATTR_BOARD_MODE)
            {
                is_board_mode_existed = true;
                board_mode = (lai_linecard_board_mode_t)(attrList[idx].value.s32);
            }
            else
            {
                /*
                 * Those attributes can be OID attributes, so we need to
                 * process them after creating linecard.
                 */

                attrs_left.push_back(attrList[idx]); // struct copy, we will keep the same pointers
                attr_count_left++;
            }
        }

        lai_attribute_t* attr_list = attrs.data();

        SWSS_LOG_INFO("creating linecard VID: %s", lai_serialize_object_id(m_linecard_vid).c_str());

        lai_status_t status;

        {
            SWSS_LOG_TIMER("Cold boot: create linecard");
            status = m_vendorLai->create(LAI_OBJECT_TYPE_LINECARD, &m_linecard_rid, 0, attr_count, attr_list);
        }

        if (status != LAI_STATUS_SUCCESS)
        {
            SWSS_LOG_THROW("failed to create linecard RID: %s",
                lai_serialize_status(status).c_str());
        }

        SWSS_LOG_NOTICE("created linecard RID: %s",
            lai_serialize_object_id(m_linecard_rid).c_str());
        /*
         * Save this linecard ids as translated.
         */

        m_translatedV2R[m_linecard_vid] = m_linecard_rid;
        m_translatedR2V[m_linecard_rid] = m_linecard_vid;

        /*
         * LaiLinecard class object must be created before before any other
         * object, so when doing discover we will get full default ASIC view.
         */

        m_sw = std::make_shared<LaiLinecard>(m_linecard_vid, m_linecard_rid, m_client, m_translator, m_vendorLai);

        lai_attribute_t pre_config_attr;
        pre_config_attr.id = LAI_LINECARD_ATTR_START_PRE_CONFIGURATION;
        pre_config_attr.value.booldata = true;

        status = m_vendorLai->set(LAI_OBJECT_TYPE_LINECARD, m_linecard_rid, &pre_config_attr);
        if (status != LAI_STATUS_SUCCESS) {
            SWSS_LOG_THROW("failed to start pre-config linecard");
        }

        if (is_board_mode_existed) {
            setBoardMode(board_mode);
        }
       
        /*
         * We processed linecard. We have linecard vid/rid so we can process all
         * other attributes of linecards that are not mandatory on create and are
         * not crate only.
         *
         * Since those left attributes may contain VIDs we need to process
         * attributes for oids.
         */

        processAttributesForOids(LAI_OBJECT_TYPE_LINECARD, attr_count_left, attrs_left.data());

        for (uint32_t idx = 0; idx < attr_count_left; ++idx)
        {
            lai_attribute_t* attr = &attrs_left[idx];

            status = m_vendorLai->set(LAI_OBJECT_TYPE_LINECARD, m_linecard_rid, attr);

            if (status != LAI_STATUS_SUCCESS)
            {
                SWSS_LOG_THROW("failed to set attribute %s on linecard VID %s: %s",
                    lai_metadata_get_attr_metadata(LAI_OBJECT_TYPE_LINECARD, attr->id)->attridname,
                    lai_serialize_object_id(m_linecard_rid).c_str(),
                    lai_serialize_status(status).c_str());
            }
        }
    }
}

void SingleReiniter::setBoardMode(lai_linecard_board_mode_t mode)
{
    SWSS_LOG_ENTER();

    int wait_count = 0;
    lai_attribute_t attr;
    lai_status_t status;

    attr.id = LAI_LINECARD_ATTR_BOARD_MODE;
    status = m_vendorLai->get(LAI_OBJECT_TYPE_LINECARD, m_linecard_rid, 1, &attr);
    if (status == LAI_STATUS_SUCCESS && attr.value.s32 == mode)
    {
        SWSS_LOG_DEBUG("Linecard and maincard have a same board-mode, %d", mode);
        return;
    }

    SWSS_LOG_NOTICE("Begin to set board-mode %d", mode);

    attr.value.s32 = mode;
    status = m_vendorLai->set(LAI_OBJECT_TYPE_LINECARD, m_linecard_rid, &attr);
    if (status != LAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set board-mode status=%d, mode=%d",
                       status, mode);
        return;
    }
    do
    {
        wait_count++;
        this_thread::sleep_for(chrono::milliseconds(1000));
        status = m_vendorLai->get(LAI_OBJECT_TYPE_LINECARD, m_linecard_rid, 1, &attr);
        if (status != LAI_STATUS_SUCCESS)
        {
            continue;
        }
        if (attr.value.s32 == mode)
        {
            break;
        }
    } while (wait_count < 10 * 60); /* 10 minutes is enough for P230C to change its boardmode */

    SWSS_LOG_NOTICE("The end of setting board-mode");
}

void SingleReiniter::listFailedAttributes(
    _In_ lai_object_type_t objectType,
    _In_ uint32_t attrCount,
    _In_ const lai_attribute_t* attrList)
{
    SWSS_LOG_ENTER();

    for (uint32_t idx = 0; idx < attrCount; idx++)
    {
        const lai_attribute_t* attr = &attrList[idx];

        auto meta = lai_metadata_get_attr_metadata(objectType, attr->id);

        if (meta == NULL)
        {
            SWSS_LOG_ERROR("failed to get attribute metadata %s %d",
                lai_serialize_object_type(objectType).c_str(),
                attr->id);

            continue;
        }

        SWSS_LOG_ERROR("%s = %s", meta->attridname, lai_serialize_attr_value(*meta, *attr).c_str());
    }
}

lai_object_id_t SingleReiniter::processSingleVid(
    _In_ lai_object_id_t vid)
{
    SWSS_LOG_ENTER();

    if (vid == LAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_DEBUG("processed VID 0 to RID 0");

        return LAI_NULL_OBJECT_ID;
    }

    auto it = m_translatedV2R.find(vid);

    if (it != m_translatedV2R.end())
    {
        /*
         * This object was already processed, just return real object id.
         */

        SWSS_LOG_DEBUG("processed VID %s to RID %s",
            lai_serialize_object_id(vid).c_str(),
            lai_serialize_object_id(it->second).c_str());

        return it->second;
    }

    lai_object_type_t objectType = VidManager::objectTypeQuery(vid);

    std::string strVid = lai_serialize_object_id(vid);

    auto oit = m_oids.find(strVid);

    if (oit == m_oids.end())
    {
        SWSS_LOG_THROW("failed to find VID %s in OIDs map", strVid.c_str());
    }

    std::string asicKey = oit->second;

    std::shared_ptr<LaiAttributeList> list = m_attributesLists[asicKey];

    lai_attribute_t* attrList = list->get_attr_list();

    uint32_t attrCount = list->get_attr_count();

    m_handler->updateNotificationsPointers(objectType, attrCount, attrList);

    processAttributesForOids(objectType, attrCount, attrList);

    bool createObject = true;

    /*
     * Now let's determine whether this object need to be created.  Default
     * objects like default virtual router, queues or cpu can't be created.
     * When object exists on the switch (even VLAN member) it will not be
     * created, but matched. We just need to watch for RO/CO attributes.
     *
     * NOTE: this also should be per linecard.
     */

    auto v2rMapIt = m_vidToRidMap.find(vid);

    if (v2rMapIt == m_vidToRidMap.end())
    {
        SWSS_LOG_THROW("failed to find VID %s in VIDTORID map",
            lai_serialize_object_id(vid).c_str());
    }

    lai_object_id_t rid;

    if (m_sw->isDiscoveredRid(v2rMapIt->second))
    {
        rid = v2rMapIt->second;

        createObject = false;

        SWSS_LOG_DEBUG("object %s will not be created, processed VID %s to RID %s",
            lai_serialize_object_type(objectType).c_str(),
            lai_serialize_object_id(vid).c_str(),
            lai_serialize_object_id(rid).c_str());
    }
    ///////////////////////////////////////////////////////////////////////////////////////////////////////
    uint32_t attr_count = 0;            // attr count needed for create
    uint32_t attr_count_left = 0;       // attr count after create

    std::vector<lai_attribute_t> attrs;         // attrs for create
    std::vector<lai_attribute_t> attrs_left;    // attrs for set

    for (uint32_t idx = 0; idx < attrCount; ++idx)
    {
        auto meta = lai_metadata_get_attr_metadata(objectType, attrList[idx].id);

        if (LAI_HAS_FLAG_MANDATORY_ON_CREATE(meta->flags) || LAI_HAS_FLAG_CREATE_ONLY(meta->flags))
        {
            /*
             * If attribute is mandatory on create or create only, we need
             * to select it for linecard create method, since it's required
             * on create or it will not be possible to change it after
             * create.
             *
             * Currently linecard don't have any conditional attributes but
             * we could take this into account. Even if any of those
             * conditional attributes will present, it will be not be oid
             * attribute.
             */

            attrs.push_back(attrList[idx]); // struct copy, we will keep the same pointers
            attr_count++;
        }
        else
        {
            /*
             * Those attributes can be OID attributes, so we need to
             * process them after creating linecard.
             */

            attrs_left.push_back(attrList[idx]); // struct copy, we will keep the same pointers
            attr_count_left++;
        }
    }
    SWSS_LOG_INFO("processSingleVid setting attributes on object of type %x, processed VID 0x%" PRIx64 " to RID 0x%" PRIx64 " attr_count %d, attr_count_left %d", objectType, vid, rid, attr_count, attr_count_left);
    ///////////////////////////////////////////////////////////////////////////////////////////////////////

    if (createObject)
    {
        lai_object_meta_key_t meta_key;

        meta_key.objecttype = objectType;

        /*
         * Since we have only one linecard, we can get away using m_linecard_rid here.
         */

#ifdef ENABLE_PERF
        auto start = std::chrono::high_resolution_clock::now();
#endif

        lai_status_t status = m_vendorLai->create(meta_key.objecttype, &meta_key.objectkey.key.object_id, m_linecard_rid, attr_count, attrs.data());

#ifdef ENABLE_PERF
        auto end = std::chrono::high_resolution_clock::now();

        typedef std::chrono::duration<double, std::ratio<1>> second_t;

        double duration = std::chrono::duration_cast<second_t>(end - start).count();

        std::get<0>(m_perf_create[objectType])++;
        std::get<1>(m_perf_create[objectType]) += duration;
#endif

        if (status != LAI_STATUS_SUCCESS)
        {
            listFailedAttributes(objectType, attr_count, attrs.data());

            SWSS_LOG_THROW("failed to create object %s: %s",
                lai_serialize_object_type(objectType).c_str(),
                lai_serialize_status(status).c_str());
        }

        rid = meta_key.objectkey.key.object_id;

        SWSS_LOG_DEBUG("created object of type %s, processed VID %s to RID %s",
            lai_serialize_object_type(objectType).c_str(),
            lai_serialize_object_id(vid).c_str(),
            lai_serialize_object_id(rid).c_str());
    }
    else
    {
        SWSS_LOG_DEBUG("setting attributes on object of type %x, processed VID 0x%" PRIx64 " to RID 0x%" PRIx64 " ", objectType, vid, rid);

        for (uint32_t idx = 0; idx < attr_count_left; idx++)
        {
            lai_attribute_t* attr = &attrs_left[idx];

            auto meta = lai_metadata_get_attr_metadata(objectType, attr->id);

            if (meta == NULL)
            {
                SWSS_LOG_THROW("failed to get attribute metadata %s: %d",
                    lai_serialize_object_type(objectType).c_str(),
                    attr->id);
            }

            if (LAI_HAS_FLAG_CREATE_ONLY(meta->flags))
            {
                /*
                 * If we will be performing this on default existing created
                 * object then it may happen that during snoop in previous
                 * iteration we put some attribute that is create only, then
                 * this set will fail and we need to skip this set.
                 *
                 * NOTE: We could do get here to see if it actually matches.
                 */

                if (m_sw->isDiscoveredRid(rid))
                {
                    continue;
                }

                SWSS_LOG_WARN("skipping create only attr %s: %s",
                    meta->attridname,
                    lai_serialize_attr_value(*meta, *attr).c_str());

                continue;
            }

#ifdef ENABLE_PERF
            auto start = std::chrono::high_resolution_clock::now();
#endif

            lai_status_t status = m_vendorLai->set(objectType, rid, attr);

#ifdef ENABLE_PERF
            auto end = std::chrono::high_resolution_clock::now();

            typedef std::chrono::duration<double, std::ratio<1>> second_t;

            double duration = std::chrono::duration_cast<second_t>(end - start).count();

            std::get<0>(m_perf_set[objectType])++;
            std::get<1>(m_perf_set[objectType]) += duration;
#endif

            if (status != LAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR(
                    "failed to set %s value %s: %s",
                    meta->attridname,
                    lai_serialize_attr_value(*meta, *attr).c_str(),
                    lai_serialize_status(status).c_str());
            }
        }
    }

    m_translatedV2R[vid] = rid;
    m_translatedR2V[rid] = vid;

    return rid;
}

void SingleReiniter::processAttributesForOids(
    _In_ lai_object_type_t objectType,
    _In_ uint32_t attr_count,
    _In_ lai_attribute_t* attr_list)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_DEBUG("processing list for object type %s",
        lai_serialize_object_type(objectType).c_str());

    for (uint32_t idx = 0; idx < attr_count; idx++)
    {
        lai_attribute_t& attr = attr_list[idx];

        auto meta = lai_metadata_get_attr_metadata(objectType, attr.id);

        if (meta == NULL)
        {
            SWSS_LOG_THROW("unable to get metadata for object type %s, attribute %d",
                lai_serialize_object_type(objectType).c_str(),
                attr.id);
        }

        uint32_t count = 0;
        lai_object_id_t* objectIdList;

        switch (meta->attrvaluetype)
        {
        case LAI_ATTR_VALUE_TYPE_OBJECT_ID:
            count = 1;
            objectIdList = &attr.value.oid;
            break;

        case LAI_ATTR_VALUE_TYPE_OBJECT_LIST:
            count = attr.value.objlist.count;
            objectIdList = attr.value.objlist.list;
            break;

        default:

            // TODO later isoidattribute
            if (meta->allowedobjecttypeslength > 0)
            {
                SWSS_LOG_THROW("attribute %s is oid attribute, but not processed, FIXME", meta->attridname);
            }

            /*
             * This is not oid attribute, we can skip processing.
             */

            continue;
        }

        /*
         * Attribute contains object id's, they need to be translated some of
         * them could be already translated.
         */

        for (uint32_t j = 0; j < count; j++)
        {
            lai_object_id_t vid = objectIdList[j];

            lai_object_id_t rid = processSingleVid(vid);

            objectIdList[j] = rid;
        }
    }
}

void SingleReiniter::processOids()
{
    SWSS_LOG_ENTER();

    for (const auto& kv : m_oids)
    {
        const std::string& strObjectId = kv.first;

        lai_object_id_t vid;
        lai_deserialize_object_id(strObjectId, vid);

        processSingleVid(vid);
    }
}

void SingleReiniter::processStructNonObjectIds(
    _In_ lai_object_meta_key_t& meta_key)
{
    SWSS_LOG_ENTER();

    auto info = lai_metadata_get_object_type_info(meta_key.objecttype);

    /*
     * Call processSingleVid method for each oid in non object id (struct
     * entry) in generic way.
     */

    if (info->isnonobjectid)
    {
        for (size_t j = 0; j < info->structmemberscount; ++j)
        {
            const lai_struct_member_info_t* m = info->structmembers[j];

            if (m->membervaluetype != LAI_ATTR_VALUE_TYPE_OBJECT_ID)
            {
                continue;
            }

            lai_object_id_t vid = m->getoid(&meta_key);

            lai_object_id_t rid = processSingleVid(vid);

            m->setoid(&meta_key, rid);

            SWSS_LOG_DEBUG("processed vid 0x%" PRIx64 " to rid 0x%" PRIx64 " in %s:%s", vid, rid,
                info->objecttypename,
                m->membername);
        }
    }
}

void SingleReiniter::checkAllIds()
{
    SWSS_LOG_ENTER();

    for (auto& kv : m_translatedV2R)
    {
        auto it = m_vidToRidMap.find(kv.first);

        if (it == m_vidToRidMap.end())
        {
            SWSS_LOG_THROW("failed to find vid %s in previous map",
                lai_serialize_object_id(kv.first).c_str());
        }

        m_vidToRidMap.erase(it);
    }

    size_t size = m_vidToRidMap.size();

    if (size != 0)
    {
        for (auto& kv : m_vidToRidMap)
        {
            lai_object_type_t objectType = VidManager::objectTypeQuery(kv.first);

            SWSS_LOG_ERROR("vid not translated: %s, object type: %s",
                lai_serialize_object_id(kv.first).c_str(),
                lai_serialize_object_type(objectType).c_str());
        }

        SWSS_LOG_THROW("vid to rid map is not empty (%zu) after translation", size);
    }
}

SingleReiniter::ObjectIdMap SingleReiniter::getTranslatedVid2Rid() const
{
    SWSS_LOG_ENTER();

    return m_translatedV2R;
}

void SingleReiniter::postRemoveActions()
{
    SWSS_LOG_ENTER();

    /*
     * Now we must check whether we need to remove some objects like VLAN
     * members etc.
     *
     * TODO: Should this be done at start, before other operations?
     * We are able to determine which objects are missing from rid map
     * as long as id's between restart don't change.
     */

    if (m_sw == nullptr)
    {
        /*
         * No linecard was created.
         */

        return;
    }
}

std::shared_ptr<LaiAttributeList> SingleReiniter::redisGetAttributesFromAsicKey(
    _In_ const std::string& key)
{
    SWSS_LOG_ENTER();

    lai_object_type_t objectType = getObjectTypeFromAsicKey(key);

    std::vector<swss::FieldValueTuple> values;

    auto hash = m_client->getAttributesFromAsicKey(key);

    for (auto& kv : hash)
    {
        const std::string& skey = kv.first;
        const std::string& svalue = kv.second;

        swss::FieldValueTuple fvt(skey, svalue);

        values.push_back(fvt);
    }

    return std::make_shared<LaiAttributeList>(objectType, values, false);
}

std::shared_ptr<LaiLinecard> SingleReiniter::getLinecard() const
{
    SWSS_LOG_ENTER();

    return m_sw;
}

