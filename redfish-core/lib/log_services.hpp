/*
// Copyright (c) 2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/
#pragma once

#include "http_utility.hpp"
#include "registries.hpp"
#include "registries/base_message_registry.hpp"
#include "registries/openbmc_message_registry.hpp"
#include "task.hpp"

#include <systemd/sd-journal.h>
#include <unistd.h>

#include <app.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/beast/core/span.hpp>
#include <boost/beast/http.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/system/linux_error.hpp>
#include <error_messages.hpp>
#include <registries/privilege_registry.hpp>
#include <utils/error_log_utils.hpp>

#include <charconv>
#include <filesystem>
#include <optional>
#include <string_view>
#include <tuple>
#include <variant>

namespace redfish
{

constexpr char const* crashdumpObject = "com.intel.crashdump";
constexpr char const* crashdumpPath = "/com/intel/crashdump";
constexpr char const* crashdumpInterface = "com.intel.crashdump";
constexpr char const* deleteAllInterface =
    "xyz.openbmc_project.Collection.DeleteAll";
constexpr char const* crashdumpOnDemandInterface =
    "com.intel.crashdump.OnDemand";
constexpr char const* crashdumpTelemetryInterface =
    "com.intel.crashdump.Telemetry";

#ifdef BMCWEB_ENABLE_HW_ISOLATION
constexpr std::array<const char*, 3> hwIsolationEntryIfaces = {
    "xyz.openbmc_project.HardwareIsolation.Entry",
    "xyz.openbmc_project.Association.Definitions",
    "xyz.openbmc_project.Time.EpochTime"};

using RedfishResourceDBusInterfaces = std::string;
using RedfishResourceCollectionUri = std::string;
using RedfishUriListType = std::unordered_map<RedfishResourceDBusInterfaces,
                                              RedfishResourceCollectionUri>;

RedfishUriListType redfishUriList = {
    {"xyz.openbmc_project.Inventory.Item.Cpu",
     "/redfish/v1/Systems/system/Processors"},
    {"xyz.openbmc_project.Inventory.Item.Dimm",
     "/redfish/v1/Systems/system/Memory"},
    {"xyz.openbmc_project.Inventory.Item.CpuCore",
     "/redfish/v1/Systems/system/Processors/<str>/SubProcessors"},
    {"xyz.openbmc_project.Inventory.Item.Chassis", "/redfish/v1/Chassis"},
    {"xyz.openbmc_project.Inventory.Item.Tpm",
     "/redfish/v1/Chassis/<str>/Assembly#/Assemblies"},
    {"xyz.openbmc_project.Inventory.Item.Board.Motherboard",
     "/redfish/v1/Chassis/<str>/Assembly#/Assemblies"}};

#endif // BMCWEB_ENABLE_HW_ISOLATION

enum class eventLogTypes
{
    eventLog = 1,
    ceLog,
};

enum DumpCreationProgress
{
    DUMP_CREATE_SUCCESS,
    DUMP_CREATE_FAILED,
    DUMP_CREATE_INPROGRESS
};

namespace message_registries
{
static const Message* getMessageFromRegistry(
    const std::string& messageKey,
    const boost::beast::span<const MessageEntry> registry)
{
    boost::beast::span<const MessageEntry>::const_iterator messageIt =
        std::find_if(registry.cbegin(), registry.cend(),
                     [&messageKey](const MessageEntry& messageEntry) {
                         return !std::strcmp(messageEntry.first,
                                             messageKey.c_str());
                     });
    if (messageIt != registry.cend())
    {
        return &messageIt->second;
    }

    return nullptr;
}

static const Message* getMessage(const std::string_view& messageID)
{
    // Redfish MessageIds are in the form
    // RegistryName.MajorVersion.MinorVersion.MessageKey, so parse it to find
    // the right Message
    std::vector<std::string> fields;
    fields.reserve(4);
    boost::split(fields, messageID, boost::is_any_of("."));
    std::string& registryName = fields[0];
    std::string& messageKey = fields[3];

    // Find the right registry and check it for the MessageKey
    if (std::string(base::header.registryPrefix) == registryName)
    {
        return getMessageFromRegistry(
            messageKey, boost::beast::span<const MessageEntry>(base::registry));
    }
    if (std::string(openbmc::header.registryPrefix) == registryName)
    {
        return getMessageFromRegistry(
            messageKey,
            boost::beast::span<const MessageEntry>(openbmc::registry));
    }
    return nullptr;
}
} // namespace message_registries

namespace fs = std::filesystem;

using AssociationsValType =
    std::vector<std::tuple<std::string, std::string, std::string>>;
using GetManagedPropertyType = boost::container::flat_map<
    std::string,
    std::variant<std::string, bool, uint8_t, int16_t, uint16_t, int32_t,
                 uint32_t, int64_t, uint64_t, double, AssociationsValType>>;

using GetManagedObjectsType = boost::container::flat_map<
    sdbusplus::message::object_path,
    boost::container::flat_map<std::string, GetManagedPropertyType>>;

inline std::string translateSeverityDbusToRedfish(const std::string& s)
{
    if ((s == "xyz.openbmc_project.Logging.Entry.Level.Alert") ||
        (s == "xyz.openbmc_project.Logging.Entry.Level.Critical") ||
        (s == "xyz.openbmc_project.Logging.Entry.Level.Emergency") ||
        (s == "xyz.openbmc_project.Logging.Entry.Level.Error"))
    {
        return "Critical";
    }
    if ((s == "xyz.openbmc_project.Logging.Entry.Level.Debug") ||
        (s == "xyz.openbmc_project.Logging.Entry.Level.Informational") ||
        (s == "xyz.openbmc_project.Logging.Entry.Level.Notice"))
    {
        return "OK";
    }
    if (s == "xyz.openbmc_project.Logging.Entry.Level.Warning")
    {
        return "Warning";
    }
    return "";
}

inline static int getJournalMetadata(sd_journal* journal,
                                     const std::string_view& field,
                                     std::string_view& contents)
{
    const char* data = nullptr;
    size_t length = 0;
    int ret = 0;
    // Get the metadata from the requested field of the journal entry
    ret = sd_journal_get_data(journal, field.data(),
                              reinterpret_cast<const void**>(&data), &length);
    if (ret < 0)
    {
        return ret;
    }
    contents = std::string_view(data, length);
    // Only use the content after the "=" character.
    contents.remove_prefix(std::min(contents.find('=') + 1, contents.size()));
    return ret;
}

inline static int getJournalMetadata(sd_journal* journal,
                                     const std::string_view& field,
                                     const int& base, long int& contents)
{
    int ret = 0;
    std::string_view metadata;
    // Get the metadata from the requested field of the journal entry
    ret = getJournalMetadata(journal, field, metadata);
    if (ret < 0)
    {
        return ret;
    }
    contents = strtol(metadata.data(), nullptr, base);
    return ret;
}

inline static bool getEntryTimestamp(sd_journal* journal,
                                     std::string& entryTimestamp)
{
    int ret = 0;
    uint64_t timestamp = 0;
    ret = sd_journal_get_realtime_usec(journal, &timestamp);
    if (ret < 0)
    {
        BMCWEB_LOG_ERROR << "Failed to read entry timestamp: "
                         << strerror(-ret);
        return false;
    }
    entryTimestamp = crow::utility::getDateTime(
        static_cast<std::time_t>(timestamp / 1000 / 1000));
    return true;
}

static bool getSkipParam(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                         const crow::Request& req, uint64_t& skip)
{
    boost::urls::query_params_view::iterator it = req.urlParams.find("$skip");
    if (it != req.urlParams.end())
    {
        std::string skipParam = it->value();
        char* ptr = nullptr;
        skip = std::strtoul(skipParam.c_str(), &ptr, 10);
        if (skipParam.empty() || *ptr != '\0')
        {

            messages::queryParameterValueTypeError(
                asyncResp->res, std::string(skipParam), "$skip");
            return false;
        }
    }
    return true;
}

static constexpr const uint64_t maxEntriesPerPage = 1000;
static bool getTopParam(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                        const crow::Request& req, uint64_t& top)
{
    boost::urls::query_params_view::iterator it = req.urlParams.find("$top");
    if (it != req.urlParams.end())
    {
        std::string topParam = it->value();
        char* ptr = nullptr;
        top = std::strtoul(topParam.c_str(), &ptr, 10);
        if (topParam.empty() || *ptr != '\0')
        {
            messages::queryParameterValueTypeError(
                asyncResp->res, std::string(topParam), "$top");
            return false;
        }
        if (top < 1U || top > maxEntriesPerPage)
        {

            messages::queryParameterOutOfRange(
                asyncResp->res, std::to_string(top), "$top",
                "1-" + std::to_string(maxEntriesPerPage));
            return false;
        }
    }
    return true;
}

inline static bool getUniqueEntryID(sd_journal* journal, std::string& entryID,
                                    const bool firstEntry = true)
{
    int ret = 0;
    static uint64_t prevTs = 0;
    static int index = 0;
    if (firstEntry)
    {
        prevTs = 0;
    }

    // Get the entry timestamp
    uint64_t curTs = 0;
    ret = sd_journal_get_realtime_usec(journal, &curTs);
    if (ret < 0)
    {
        BMCWEB_LOG_ERROR << "Failed to read entry timestamp: "
                         << strerror(-ret);
        return false;
    }
    // If the timestamp isn't unique, increment the index
    if (curTs == prevTs)
    {
        index++;
    }
    else
    {
        // Otherwise, reset it
        index = 0;
    }
    // Save the timestamp
    prevTs = curTs;

    entryID = std::to_string(curTs);
    if (index > 0)
    {
        entryID += "_" + std::to_string(index);
    }
    return true;
}

static bool getUniqueEntryID(const std::string& logEntry, std::string& entryID,
                             const bool firstEntry = true)
{
    static time_t prevTs = 0;
    static int index = 0;
    if (firstEntry)
    {
        prevTs = 0;
    }

    // Get the entry timestamp
    std::time_t curTs = 0;
    std::tm timeStruct = {};
    std::istringstream entryStream(logEntry);
    if (entryStream >> std::get_time(&timeStruct, "%Y-%m-%dT%H:%M:%S"))
    {
        curTs = std::mktime(&timeStruct);
    }
    // If the timestamp isn't unique, increment the index
    if (curTs == prevTs)
    {
        index++;
    }
    else
    {
        // Otherwise, reset it
        index = 0;
    }
    // Save the timestamp
    prevTs = curTs;

    entryID = std::to_string(curTs);
    if (index > 0)
    {
        entryID += "_" + std::to_string(index);
    }
    return true;
}

inline static bool
    getTimestampFromID(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                       const std::string& entryID, uint64_t& timestamp,
                       uint64_t& index)
{
    if (entryID.empty())
    {
        return false;
    }
    // Convert the unique ID back to a timestamp to find the entry
    std::string_view tsStr(entryID);

    auto underscorePos = tsStr.find('_');
    if (underscorePos != tsStr.npos)
    {
        // Timestamp has an index
        tsStr.remove_suffix(tsStr.size() - underscorePos);
        std::string_view indexStr(entryID);
        indexStr.remove_prefix(underscorePos + 1);
        auto [ptr, ec] = std::from_chars(
            indexStr.data(), indexStr.data() + indexStr.size(), index);
        if (ec != std::errc())
        {
            messages::resourceMissingAtURI(asyncResp->res, entryID);
            return false;
        }
    }
    // Timestamp has no index
    auto [ptr, ec] =
        std::from_chars(tsStr.data(), tsStr.data() + tsStr.size(), timestamp);
    if (ec != std::errc())
    {
        messages::resourceMissingAtURI(asyncResp->res, entryID);
        return false;
    }
    return true;
}

static bool
    getRedfishLogFiles(std::vector<std::filesystem::path>& redfishLogFiles)
{
    static const std::filesystem::path redfishLogDir = "/var/log";
    static const std::string redfishLogFilename = "redfish";

    // Loop through the directory looking for redfish log files
    for (const std::filesystem::directory_entry& dirEnt :
         std::filesystem::directory_iterator(redfishLogDir))
    {
        // If we find a redfish log file, save the path
        std::string filename = dirEnt.path().filename();
        if (boost::starts_with(filename, redfishLogFilename))
        {
            redfishLogFiles.emplace_back(redfishLogDir / filename);
        }
    }
    // As the log files rotate, they are appended with a ".#" that is higher for
    // the older logs. Since we don't expect more than 10 log files, we
    // can just sort the list to get them in order from newest to oldest
    std::sort(redfishLogFiles.begin(), redfishLogFiles.end());

    return !redfishLogFiles.empty();
}

inline void
    getDumpEntryCollection(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                           const std::string& dumpType)
{
    std::string dumpPath;
    if (dumpType == "BMC")
    {
        dumpPath = "/redfish/v1/Managers/bmc/LogServices/Dump/Entries/";
    }
    else if (dumpType == "System" || dumpType == "Resource" ||
             dumpType == "Hostboot" || dumpType == "Hardware" ||
             dumpType == "SBE")
    {
        dumpPath = "/redfish/v1/Systems/system/LogServices/Dump/Entries/";
    }
    else
    {
        BMCWEB_LOG_ERROR << "Invalid dump type" << dumpType;
        messages::internalError(asyncResp->res);
        return;
    }

    crow::connections::systemBus->async_method_call(
        [asyncResp, dumpPath, dumpType](const boost::system::error_code ec,
                                        GetManagedObjectsType& resp) {
            if (ec)
            {
                BMCWEB_LOG_ERROR << "DumpEntry resp_handler got error " << ec;
                messages::internalError(asyncResp->res);
                return;
            }

            nlohmann::json& entriesArray = asyncResp->res.jsonValue["Members"];
            if (!entriesArray.size())
            {
                entriesArray = nlohmann::json::array();
            }
            std::string dumpEntryPath =
                "/xyz/openbmc_project/dump/" +
                std::string(boost::algorithm::to_lower_copy(dumpType)) +
                "/entry/";

            for (auto& object : resp)
            {
                if (object.first.str.find(dumpEntryPath) == std::string::npos)
                {
                    continue;
                }
                std::time_t timestamp;
                uint64_t size = 0;
                std::string dumpStatus;
                std::string clientId;
                nlohmann::json thisEntry;

                std::string entryID = object.first.filename();
                if (entryID.empty())
                {
                    continue;
                }

                for (auto& interfaceMap : object.second)
                {
                    if (interfaceMap.first ==
                        "xyz.openbmc_project.Common.Progress")
                    {
                        for (auto& propertyMap : interfaceMap.second)
                        {
                            if (propertyMap.first == "Status")
                            {
                                auto status = std::get_if<std::string>(
                                    &propertyMap.second);
                                if (status == nullptr)
                                {
                                    messages::internalError(asyncResp->res);
                                    break;
                                }
                                dumpStatus = *status;
                            }
                        }
                    }
                    else if (interfaceMap.first ==
                             "xyz.openbmc_project.Dump.Entry")
                    {

                        for (auto& propertyMap : interfaceMap.second)
                        {
                            if (propertyMap.first == "Size")
                            {
                                auto sizePtr =
                                    std::get_if<uint64_t>(&propertyMap.second);
                                if (sizePtr == nullptr)
                                {
                                    messages::internalError(asyncResp->res);
                                    break;
                                }
                                size = *sizePtr;
                                break;
                            }
                        }
                    }
                    else if (interfaceMap.first ==
                             "xyz.openbmc_project.Time.EpochTime")
                    {

                        for (auto& propertyMap : interfaceMap.second)
                        {
                            if (propertyMap.first == "Elapsed")
                            {
                                const uint64_t* usecsTimeStamp =
                                    std::get_if<uint64_t>(&propertyMap.second);
                                if (usecsTimeStamp == nullptr)
                                {
                                    messages::internalError(asyncResp->res);
                                    break;
                                }
                                timestamp = static_cast<std::time_t>(
                                    *usecsTimeStamp / 1000 / 1000);
                                break;
                            }
                        }
                    }
                    else if (interfaceMap.first ==
                             "xyz.openbmc_project.Common.GeneratedBy")
                    {
                        for (auto& propertyMap : interfaceMap.second)
                        {
                            if (propertyMap.first == "GeneratorId")
                            {
                                const std::string* id =
                                    std::get_if<std::string>(
                                        &propertyMap.second);
                                if (id == nullptr)
                                {
                                    messages::internalError(asyncResp->res);
                                    break;
                                }
                                clientId = *id;
                                break;
                            }
                        }
                    }
                }

                if (dumpStatus != "xyz.openbmc_project.Common.Progress."
                                  "OperationStatus.Completed" &&
                    !dumpStatus.empty())
                {
                    // Dump status is not Complete, no need to enumerate
                    continue;
                }

                thisEntry["@odata.type"] = "#LogEntry.v1_8_0.LogEntry";
                thisEntry["@odata.id"] = dumpPath + entryID;
                thisEntry["Id"] = entryID;
                thisEntry["EntryType"] = "Event";
                thisEntry["Created"] = crow::utility::getDateTime(timestamp);

                thisEntry["AdditionalDataSizeBytes"] = size;

                if (!clientId.empty())
                {
                    thisEntry["Oem"]["OpenBMC"]["@odata.type"] =
                        "#OemLogEntry.v1_0_0.LogEntry";
                    thisEntry["Oem"]["OpenBMC"]["GeneratorId"] = clientId;
                }

                if (dumpType == "BMC")
                {
                    thisEntry["@odata.id"] = dumpPath + entryID;
                    thisEntry["Id"] = entryID;
                    thisEntry["Name"] = dumpType + " Dump Entry";
                    thisEntry["DiagnosticDataType"] = "Manager";
                    thisEntry["AdditionalDataURI"] =
                        "/redfish/v1/Managers/bmc/LogServices/Dump/Entries/" +
                        entryID + "/attachment";
                }
                else if (dumpType == "System" || dumpType == "Resource" ||
                         dumpType == "Hostboot" || dumpType == "Hardware" ||
                         dumpType == "SBE")
                {
                    std::string dumpEntryId(dumpType + "_");
                    dumpEntryId.append(entryID);
                    thisEntry["@odata.id"] = dumpPath + dumpEntryId;
                    thisEntry["Id"] = dumpEntryId;
                    thisEntry["DiagnosticDataType"] = "OEM";
                    thisEntry["AdditionalDataURI"] =
                        "/redfish/v1/Systems/system/LogServices/Dump/Entries/" +
                        dumpEntryId + "/attachment";
                    thisEntry["Name"] = "System Dump Entry";
                    thisEntry["OEMDiagnosticDataType"] = dumpType;
                }
                entriesArray.push_back(std::move(thisEntry));
            }
            asyncResp->res.jsonValue["Members@odata.count"] =
                entriesArray.size();
        },
        "xyz.openbmc_project.Dump.Manager", "/xyz/openbmc_project/dump",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
}

inline void
    getDumpEntryById(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                     const std::string& entryID, const std::string& dumpType)
{
    std::string dumpPath;
    std::string dumpId;
    if (dumpType == "BMC")
    {
        dumpPath = "/redfish/v1/Managers/bmc/LogServices/Dump/Entries/";
        dumpId = entryID;
    }
    else if (dumpType == "System" || dumpType == "Resource" ||
             dumpType == "Hostboot" || dumpType == "Hardware" ||
             dumpType == "SBE")
    {
        dumpPath = "/redfish/v1/Systems/system/LogServices/Dump/Entries/";
        std::size_t pos = entryID.find_first_of('_');
        if (pos == std::string::npos || (pos + 1) >= entryID.length())
        {
            // Requested ID is invalid
            messages::invalidObject(asyncResp->res, "Dump Id");
            return;
        }
        dumpId = entryID.substr(pos + 1);
    }
    else
    {
        BMCWEB_LOG_ERROR << "Invalid dump type" << dumpType;
        messages::internalError(asyncResp->res);
        return;
    }

    crow::connections::systemBus->async_method_call(
        [asyncResp, entryID, dumpPath, dumpType, dumpId](
            const boost::system::error_code ec, GetManagedObjectsType& resp) {
            if (ec)
            {
                BMCWEB_LOG_ERROR << "DumpEntry resp_handler got error " << ec;
                messages::internalError(asyncResp->res);
                return;
            }

            bool foundDumpEntry = false;
            std::string clientId;
            std::string dumpEntryPath =
                "/xyz/openbmc_project/dump/" +
                std::string(boost::algorithm::to_lower_copy(dumpType)) +
                "/entry/";

            for (auto& objectPath : resp)
            {
                if (objectPath.first.str != dumpEntryPath + dumpId)
                {
                    continue;
                }

                foundDumpEntry = true;
                std::time_t timestamp;
                uint64_t size = 0;
                std::string dumpStatus;

                for (auto& interfaceMap : objectPath.second)
                {
                    if (interfaceMap.first ==
                        "xyz.openbmc_project.Common.Progress")
                    {
                        for (auto& propertyMap : interfaceMap.second)
                        {
                            if (propertyMap.first == "Status")
                            {
                                auto status = std::get_if<std::string>(
                                    &propertyMap.second);
                                if (status == nullptr)
                                {
                                    messages::internalError(asyncResp->res);
                                    break;
                                }
                                dumpStatus = *status;
                            }
                        }
                    }
                    else if (interfaceMap.first ==
                             "xyz.openbmc_project.Dump.Entry")
                    {
                        for (auto& propertyMap : interfaceMap.second)
                        {
                            if (propertyMap.first == "Size")
                            {
                                auto sizePtr =
                                    std::get_if<uint64_t>(&propertyMap.second);
                                if (sizePtr == nullptr)
                                {
                                    messages::internalError(asyncResp->res);
                                    break;
                                }
                                size = *sizePtr;
                                break;
                            }
                        }
                    }
                    else if (interfaceMap.first ==
                             "xyz.openbmc_project.Time.EpochTime")
                    {
                        for (auto& propertyMap : interfaceMap.second)
                        {
                            if (propertyMap.first == "Elapsed")
                            {
                                const uint64_t* usecsTimeStamp =
                                    std::get_if<uint64_t>(&propertyMap.second);
                                if (usecsTimeStamp == nullptr)
                                {
                                    messages::internalError(asyncResp->res);
                                    break;
                                }
                                timestamp = static_cast<std::time_t>(
                                    *usecsTimeStamp / 1000 / 1000);
                                break;
                            }
                        }
                    }
                    else if (interfaceMap.first ==
                             "xyz.openbmc_project.Common.GeneratedBy")
                    {
                        for (auto& propertyMap : interfaceMap.second)
                        {
                            if (propertyMap.first == "GeneratorId")
                            {
                                const std::string* id =
                                    std::get_if<std::string>(
                                        &propertyMap.second);
                                if (id == nullptr)
                                {
                                    messages::internalError(asyncResp->res);
                                    break;
                                }
                                clientId = *id;
                                break;
                            }
                        }
                    }
                }

                if (dumpStatus != "xyz.openbmc_project.Common.Progress."
                                  "OperationStatus.Completed" &&
                    !dumpStatus.empty())
                {
                    // Dump status is not Complete
                    // return not found until status is changed to Completed
                    messages::resourceNotFound(asyncResp->res,
                                               dumpType + " dump", entryID);
                    return;
                }

                asyncResp->res.jsonValue["@odata.type"] =
                    "#LogEntry.v1_8_0.LogEntry";
                asyncResp->res.jsonValue["@odata.id"] = dumpPath + entryID;
                asyncResp->res.jsonValue["Id"] = entryID;
                asyncResp->res.jsonValue["EntryType"] = "Event";
                asyncResp->res.jsonValue["Created"] =
                    crow::utility::getDateTime(timestamp);
                asyncResp->res.jsonValue["AdditionalDataSizeBytes"] = size;

                if (!clientId.empty())
                {
                    asyncResp->res.jsonValue["Oem"]["OpenBMC"]["@odata.type"] =
                        "#OemLogEntry.v1_0_0.LogEntry";
                    asyncResp->res.jsonValue["Oem"]["OpenBMC"]["GeneratorId"] =
                        clientId;
                }

                if (dumpType == "BMC")
                {
                    asyncResp->res.jsonValue["Name"] = "BMC Dump Entry";
                    asyncResp->res.jsonValue["DiagnosticDataType"] = "Manager";
                    asyncResp->res.jsonValue["AdditionalDataURI"] =
                        "/redfish/v1/Managers/bmc/LogServices/Dump/Entries/" +
                        entryID + "/attachment";
                }
                else if (dumpType == "System" || dumpType == "Resource" ||
                         dumpType == "Hostboot" || dumpType == "Hardware" ||
                         dumpType == "SBE")
                {
                    std::string dumpAttachment(
                        "/redfish/v1/Systems/system/LogServices/Dump/Entries/");
                    dumpAttachment.append(dumpType);
                    dumpAttachment.append("_" + dumpId);
                    dumpAttachment.append("/attachment");
                    asyncResp->res.jsonValue["Name"] = dumpType + " Dump Entry";
                    asyncResp->res.jsonValue["DiagnosticDataType"] = "OEM";
                    asyncResp->res.jsonValue["OEMDiagnosticDataType"] =
                        dumpType;
                    asyncResp->res.jsonValue["AdditionalDataURI"] =
                        dumpAttachment;
                }
            }
            if (foundDumpEntry == false)
            {
                BMCWEB_LOG_ERROR << "Can't find Dump Entry " << entryID;
                messages::resourceNotFound(asyncResp->res, dumpType + " dump",
                                           entryID);
                return;
            }
        },
        "xyz.openbmc_project.Dump.Manager", "/xyz/openbmc_project/dump",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
}

inline void deleteDumpEntry(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                            const std::string& entryID,
                            const std::string& dumpType)
{
    auto respHandler = [asyncResp,
                        entryID](const boost::system::error_code ec,
                                 const sdbusplus::message::message& msg) {
        BMCWEB_LOG_DEBUG << "Dump Entry doDelete callback: Done";
        if (ec)
        {
            if (ec.value() == EBADR)
            {
                messages::resourceNotFound(asyncResp->res, "LogEntry", entryID);
                return;
            }

            const sd_bus_error* dbusError = msg.get_error();
            if (dbusError == nullptr)
            {
                messages::internalError(asyncResp->res);
                return;
            }

            if (strcmp(dbusError->name, "xyz.openbmc_project.Common.Error."
                                        "Unavailable") == 0)
            {
                messages::serviceTemporarilyUnavailable(asyncResp->res, "1");
                return;
            }

            BMCWEB_LOG_ERROR << "Dump (DBus) doDelete respHandler got error "
                             << ec;
            messages::internalError(asyncResp->res);
            return;
        }
    };
    crow::connections::systemBus->async_method_call(
        respHandler, "xyz.openbmc_project.Dump.Manager",
        "/xyz/openbmc_project/dump/" +
            std::string(boost::algorithm::to_lower_copy(dumpType)) + "/entry/" +
            entryID,
        "xyz.openbmc_project.Object.Delete", "Delete");
}

inline DumpCreationProgress getDumpCompletionStatus(
    std::vector<std::pair<std::string, std::variant<std::string>>>& values,
    const std::shared_ptr<task::TaskData>& taskData, const std::string& objPath)
{
    for (const std::pair<std::string, std::variant<std::string>>& statusProp :
         values)
    {
        if (statusProp.first == "Status")
        {
            const std::string& value = std::get<std::string>(statusProp.second);
            if (value ==
                "xyz.openbmc_project.Common.Progress.OperationStatus.Completed")
            {
                return DUMP_CREATE_SUCCESS;
            }
            if (value == "xyz.openbmc_project.Common.Progress."
                         "OperationStatus.Failed")
            {
                return DUMP_CREATE_FAILED;
            }
            return DUMP_CREATE_INPROGRESS;
        }

        // Only resource dumps will implement the interface with this
        // property. Hence the below if statement will be hit for
        // all the resource dumps only
        if (statusProp.first == "DumpRequestStatus")
        {
            const std::string& value = std::get<std::string>(statusProp.second);
            if (value.ends_with("PermissionDenied"))
            {
                taskData->messages.emplace_back(
                    messages::insufficientPrivilege());
                return DUMP_CREATE_FAILED;
            }
            if (value.ends_with("AcfFileInvalid") ||
                value.ends_with("PasswordInvalid"))
            {
                taskData->messages.emplace_back(
                    messages::resourceAtUriUnauthorized(objPath,
                                                        "Invalid Password"));
                return DUMP_CREATE_FAILED;
            }
            if (value.ends_with("ResourceSelectorInvalid"))
            {
                taskData->messages.emplace_back(
                    messages::actionParameterUnknown("CollectDiagnosticData",
                                                     "Resource selector"));
                return DUMP_CREATE_FAILED;
            }
            if (value.ends_with("Success"))
            {
                taskData->state = "Running";
                return DUMP_CREATE_INPROGRESS;
            }
            return DUMP_CREATE_INPROGRESS;
        }
    }
    return DUMP_CREATE_INPROGRESS;
}

inline void createDumpTaskCallback(
    const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const sdbusplus::message::object_path& createdObjPath)
{
    const std::string& dumpPath = createdObjPath.parent_path().str;
    const std::string& dumpId = createdObjPath.filename();

    if (dumpPath.empty() || dumpId.empty())
    {
        BMCWEB_LOG_ERROR << "Invalid path/Id received";
        messages::internalError(asyncResp->res);
        return;
    }

    std::string dumpEntryPath;
    if (dumpPath == "/xyz/openbmc_project/dump/bmc/entry")
    {
        dumpEntryPath = "/redfish/v1/Managers/bmc/LogServices/Dump/Entries/";
    }
    else if (dumpPath == "/xyz/openbmc_project/dump/system/entry")
    {
        dumpEntryPath =
            "/redfish/v1/Systems/system/LogServices/Dump/Entries/System_";
    }
    else if (dumpPath == "/xyz/openbmc_project/dump/resource/entry")
    {
        dumpEntryPath =
            "/redfish/v1/Systems/system/LogServices/Dump/Entries/Resource_";
    }
    else
    {
        BMCWEB_LOG_ERROR << "Invalid dump type received";
        messages::internalError(asyncResp->res);
        return;
    }

    std::shared_ptr<task::TaskData> task = task::TaskData::createTask(
        [createdObjPath, dumpEntryPath,
         dumpId](boost::system::error_code err, sdbusplus::message::message& m,
                 const std::shared_ptr<task::TaskData>& taskData) {
            if (err)
            {
                BMCWEB_LOG_ERROR << createdObjPath.str
                                 << ": Error in creating dump";
                taskData->messages.emplace_back(messages::internalError());
                taskData->state = "Cancelled";
                return task::completed;
            }

            std::vector<std::pair<std::string, std::variant<std::string>>>
                values;
            std::string prop;
            m.read(prop, values);

            int dumpStatus =
                getDumpCompletionStatus(values, taskData, createdObjPath);
            if (dumpStatus == DUMP_CREATE_FAILED)
            {
                BMCWEB_LOG_ERROR << createdObjPath.str
                                 << ": Error in creating dump";
                taskData->state = "Cancelled";
                return task::completed;
            }

            if (dumpStatus == DUMP_CREATE_INPROGRESS)
            {
                BMCWEB_LOG_DEBUG << createdObjPath.str
                                 << ": Dump creation task is in progress";
                return !task::completed;
            }

            nlohmann::json retMessage = messages::success();
            taskData->messages.emplace_back(retMessage);

            std::string headerLoc =
                "Location: " + dumpEntryPath + http_helpers::urlEncode(dumpId);
            taskData->payload->httpHeaders.emplace_back(std::move(headerLoc));

            BMCWEB_LOG_CRITICAL << "INFO: " << createdObjPath.str
                                << ": Dump creation task completed";
            taskData->state = "Completed";
            return task::completed;
        },
        "type='signal',interface='org.freedesktop.DBus.Properties',"
        "member='PropertiesChanged',path='" +
            createdObjPath.str + "'");

    // Take the task state to "Running" for all dumps except
    // Resource dumps as there is no validation on the user input
    // for dump creation, meaning only in resource dump creation,
    // validation will be done on the user input.
    if (dumpPath.find("/resource/") == std::string::npos)
    {
        task->state = "Running";
    }

    task->startTimer(std::chrono::minutes(20));
    task->populateResp(asyncResp->res);
    task->payload.emplace(req);
}

inline void createDump(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                       const crow::Request& req, const std::string& dumpType)
{

    std::optional<std::string> diagnosticDataType;
    std::optional<std::string> oemDiagnosticDataType;

    if (!redfish::json_util::readJson(
            req, asyncResp->res, "DiagnosticDataType", diagnosticDataType,
            "OEMDiagnosticDataType", oemDiagnosticDataType))
    {
        return;
    }

    std::string dumpPath;
    std::vector<std::pair<std::string, std::variant<std::string, uint64_t>>>
        createDumpParams;
    if (dumpType == "System")
    {
        if (!oemDiagnosticDataType || !diagnosticDataType)
        {
            BMCWEB_LOG_ERROR << "CreateDump action parameter "
                                "'DiagnosticDataType'/"
                                "'OEMDiagnosticDataType' value not found!";
            messages::actionParameterMissing(
                asyncResp->res, "CollectDiagnosticData",
                "DiagnosticDataType & OEMDiagnosticDataType");
            return;
        }
        if (*oemDiagnosticDataType == "System")
        {
            if (*diagnosticDataType != "OEM")
            {
                BMCWEB_LOG_ERROR << "Wrong parameter values passed";
                messages::invalidObject(asyncResp->res,
                                        "System Dump creation parameters");
                return;
            }
            dumpPath = "/xyz/openbmc_project/dump/system";
        }
        else if (boost::starts_with(*oemDiagnosticDataType, "Resource"))
        {
            std::string resourceDumpType = *oemDiagnosticDataType;
            std::vector<std::variant<std::string, uint64_t>> resourceDumpParams;

            size_t pos = 0;
            while ((pos = resourceDumpType.find('_')) != std::string::npos)
            {
                resourceDumpParams.emplace_back(
                    resourceDumpType.substr(0, pos));
                if (resourceDumpParams.size() > 3)
                {
                    BMCWEB_LOG_ERROR
                        << "Invalid value for OEMDiagnosticDataType";
                    messages::invalidObject(asyncResp->res,
                                            "OEMDiagnosticDataType");
                    return;
                }
                resourceDumpType.erase(0, pos + 1);
            }
            resourceDumpParams.emplace_back(resourceDumpType);

            dumpPath = "/xyz/openbmc_project/dump/resource";

            if (resourceDumpParams.size() >= 2)
            {
                createDumpParams.emplace_back(std::make_pair(
                    "com.ibm.Dump.Create.CreateParameters.VSPString",
                    resourceDumpParams[1]));
            }

            if (resourceDumpParams.size() == 3)
            {
                createDumpParams.emplace_back(std::make_pair(
                    "com.ibm.Dump.Create.CreateParameters.Password",
                    resourceDumpParams[2]));
            }
        }
        else
        {
            BMCWEB_LOG_ERROR << "Invalid parameter values passed";
            messages::invalidObject(asyncResp->res, "Dump creation parameters");
            return;
        }
    }
    else if (dumpType == "BMC")
    {
        if (!diagnosticDataType)
        {
            BMCWEB_LOG_ERROR << "CreateDump action parameter "
                                "'DiagnosticDataType' not found!";
            messages::actionParameterMissing(
                asyncResp->res, "CollectDiagnosticData", "DiagnosticDataType");
            return;
        }
        if (*diagnosticDataType != "Manager")
        {
            BMCWEB_LOG_ERROR
                << "Wrong parameter value passed for 'DiagnosticDataType'";
            messages::invalidObject(asyncResp->res,
                                    "BMC Dump creation parameters");
            return;
        }
        dumpPath = "/xyz/openbmc_project/dump/bmc";
    }

    createDumpParams.emplace_back(std::make_pair(
        "xyz.openbmc_project.Dump.Create.CreateParameters.GeneratorId",
        req.session->clientIp));

    std::vector<std::pair<std::string, std::variant<std::string, uint64_t>>>
        createDumpParamVec(createDumpParams);

    crow::connections::systemBus->async_method_call(
        [asyncResp, req,
         dumpType](const boost::system::error_code ec,
                   const sdbusplus::message::message& msg,
                   const sdbusplus::message::object_path& objPath) {
            if (ec)
            {
                BMCWEB_LOG_ERROR << "CreateDump resp_handler got error " << ec;
                const sd_bus_error* dbusError = msg.get_error();
                if (dbusError == nullptr)
                {
                    messages::internalError(asyncResp->res);
                    return;
                }

                BMCWEB_LOG_ERROR << "CreateDump DBus error: " << dbusError->name
                                 << " and error msg: " << dbusError->message;

                if (strcmp(dbusError->name, "xyz.openbmc_project.Common.Error."
                                            "NotAllowed") == 0)
                {
                    // This will be returned as a result of createDump call
                    // made when the host is not powered on.
                    messages::resourceInStandby(asyncResp->res);
                    return;
                }

                if (strcmp(dbusError->name, "xyz.openbmc_project.Common.Error."
                                            "Unavailable") == 0)
                {
                    messages::resourceInUse(asyncResp->res);
                    return;
                }

                if (strcmp(dbusError->name,
                           "org.freedesktop.DBus.Error.NoReply") == 0)
                {
                    // This will be returned as a result of createDump call
                    // made when the dump manager is not responding.
                    messages::serviceTemporarilyUnavailable(asyncResp->res,
                                                            "60");
                    return;
                }

                if (strcmp(dbusError->name, "xyz.openbmc_project.Dump."
                                            "Create.Error.Disabled") == 0)
                {
                    std::string dumpPath;
                    if (dumpType == "BMC")
                    {
                        dumpPath = "/redfish/v1/Managers/bmc/LogServices/Dump/";
                    }
                    else if (dumpType == "System")
                    {
                        dumpPath =
                            "/redfish/v1/Systems/system/LogServices/Dump/";
                    }
                    messages::serviceDisabled(asyncResp->res, dumpPath);
                    return;
                }
                // Other Dbus errors such as:
                // xyz.openbmc_project.Common.Error.InvalidArgument &
                // org.freedesktop.DBus.Error.InvalidArgs are all related to
                // the dbus call that is made here in the bmcweb
                // implementation and has nothing to do with the client's
                // input in the request. Hence, returning internal error
                // back to the client.
                messages::internalError(asyncResp->res);
                return;
            }
            BMCWEB_LOG_DEBUG << "Dump Created. Path: " << objPath.str;
            createDumpTaskCallback(req, asyncResp, objPath);
        },
        "xyz.openbmc_project.Dump.Manager", dumpPath,
        "xyz.openbmc_project.Dump.Create", "CreateDump", createDumpParamVec);
}

inline void clearDump(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                      const std::string& dumpType)
{
    std::string dumpInterface;
    if (dumpType == "Resource" || dumpType == "Hostboot" ||
        dumpType == "Hardware" || dumpType == "SBE")
    {
        dumpInterface = "com.ibm.Dump.Entry." + dumpType;
    }
    else
    {
        dumpInterface = "xyz.openbmc_project.Dump.Entry." + dumpType;
    }

    std::string dumpTypeLowerCopy =
        std::string(boost::algorithm::to_lower_copy(dumpType));

    crow::connections::systemBus->async_method_call(
        [asyncResp, dumpType](const boost::system::error_code ec,
                              const std::vector<std::string>& subTreePaths) {
            if (ec)
            {
                BMCWEB_LOG_ERROR << "resp_handler got error " << ec;
                messages::internalError(asyncResp->res);
                return;
            }

            for (const std::string& path : subTreePaths)
            {
                sdbusplus::message::object_path objPath(path);
                std::string logID = objPath.filename();
                if (logID.empty())
                {
                    continue;
                }
                deleteDumpEntry(asyncResp, logID, dumpType);
            }
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetSubTreePaths",
        "/xyz/openbmc_project/dump/" + dumpTypeLowerCopy, 0,
        std::array<std::string, 1>{dumpInterface});
}

inline static void parseCrashdumpParameters(
    const std::vector<std::pair<std::string, VariantType>>& params,
    std::string& filename, std::string& timestamp, std::string& logfile)
{
    for (auto property : params)
    {
        if (property.first == "Timestamp")
        {
            const std::string* value =
                std::get_if<std::string>(&property.second);
            if (value != nullptr)
            {
                timestamp = *value;
            }
        }
        else if (property.first == "Filename")
        {
            const std::string* value =
                std::get_if<std::string>(&property.second);
            if (value != nullptr)
            {
                filename = *value;
            }
        }
        else if (property.first == "Log")
        {
            const std::string* value =
                std::get_if<std::string>(&property.second);
            if (value != nullptr)
            {
                logfile = *value;
            }
        }
    }
}

constexpr char const* postCodeIface = "xyz.openbmc_project.State.Boot.PostCode";
inline void requestRoutesSystemLogServiceCollection(App& app)
{
    /**
     * Functions triggers appropriate requests on DBus
     */
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/")
        .privileges(redfish::privileges::getLogServiceCollection)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request& req [[maybe_unused]],
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
                // Collections don't include the static data added by SubRoute
                // because it has a duplicate entry for members
                asyncResp->res.jsonValue["@odata.type"] =
                    "#LogServiceCollection.LogServiceCollection";
                asyncResp->res.jsonValue["@odata.id"] =
                    "/redfish/v1/Systems/system/LogServices";
                asyncResp->res.jsonValue["Name"] =
                    "System Log Services Collection";
                asyncResp->res.jsonValue["Description"] =
                    "Collection of LogServices for this Computer System";
                nlohmann::json& logServiceArray =
                    asyncResp->res.jsonValue["Members"];
                logServiceArray = nlohmann::json::array();
                logServiceArray.push_back(
                    {{"@odata.id",
                      "/redfish/v1/Systems/system/LogServices/EventLog"}});
#ifdef BMCWEB_ENABLE_REDFISH_DUMP_LOG
                logServiceArray.push_back(
                    {{"@odata.id",
                      "/redfish/v1/Systems/system/LogServices/Dump"}});
#endif

#ifdef BMCWEB_ENABLE_REDFISH_CPU_LOG
                logServiceArray.push_back(
                    {{"@odata.id",
                      "/redfish/v1/Systems/system/LogServices/Crashdump"}});
#endif

#ifdef BMCWEB_ENABLE_REDFISH_DBUS_LOG_ENTRIES
                Privileges effectiveUserPrivileges =
                    redfish::getUserPrivileges(req.userRole);

                if (isOperationAllowedWithPrivileges({{"ConfigureManager"}},
                                                     effectiveUserPrivileges))
                {
                    logServiceArray.push_back(
                        {{"@odata.id", "/redfish/v1/Systems/system/LogServices/"
                                       "CELog"}});
                }
#endif

                asyncResp->res.jsonValue["Members@odata.count"] =
                    logServiceArray.size();

                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec,
                                const std::vector<std::string>& subtreePath) {
                        if (ec)
                        {
                            BMCWEB_LOG_ERROR << ec;
                            return;
                        }

                        for (auto& pathStr : subtreePath)
                        {
                            if (pathStr.find("PostCode") != std::string::npos)
                            {
                                nlohmann::json& logServiceArrayLocal =
                                    asyncResp->res.jsonValue["Members"];
                                logServiceArrayLocal.push_back(
                                    {{"@odata.id", "/redfish/v1/Systems/system/"
                                                   "LogServices/PostCodes"}});
                                asyncResp->res
                                    .jsonValue["Members@odata.count"] =
                                    logServiceArrayLocal.size();
                                return;
                            }
                        }
                    },
                    "xyz.openbmc_project.ObjectMapper",
                    "/xyz/openbmc_project/object_mapper",
                    "xyz.openbmc_project.ObjectMapper", "GetSubTreePaths", "/",
                    0, std::array<const char*, 1>{postCodeIface});

#ifdef BMCWEB_ENABLE_HW_ISOLATION
                nlohmann::json& logServiceArrayLocal =
                    asyncResp->res.jsonValue["Members"];
                logServiceArrayLocal.push_back(
                    {{"@odata.id", "/redfish/v1/Systems/system/"
                                   "LogServices/HardwareIsolation"}});
                asyncResp->res.jsonValue["Members@odata.count"] =
                    logServiceArrayLocal.size();
#endif // BMCWEB_ENABLE_HW_ISOLATION
            });
}

inline void requestRoutesEventLogService(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/EventLog/")
        .privileges(redfish::privileges::getLogService)
        .methods(
            boost::beast::http::verb::
                get)([](const crow::Request&,
                        const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
            asyncResp->res.jsonValue["@odata.id"] =
                "/redfish/v1/Systems/system/LogServices/EventLog";
            asyncResp->res.jsonValue["@odata.type"] =
                "#LogService.v1_1_0.LogService";
            asyncResp->res.jsonValue["Name"] = "Event Log Service";
            asyncResp->res.jsonValue["Description"] =
                "System Event Log Service";
            asyncResp->res.jsonValue["Id"] = "EventLog";
            asyncResp->res.jsonValue["OverWritePolicy"] = "WrapsWhenFull";

            std::pair<std::string, std::string> redfishDateTimeOffset =
                crow::utility::getDateTimeOffsetNow();

            asyncResp->res.jsonValue["DateTime"] = redfishDateTimeOffset.first;
            asyncResp->res.jsonValue["DateTimeLocalOffset"] =
                redfishDateTimeOffset.second;

            asyncResp->res.jsonValue["Entries"] = {
                {"@odata.id",
                 "/redfish/v1/Systems/system/LogServices/EventLog/Entries"}};
            asyncResp->res.jsonValue["Actions"]["#LogService.ClearLog"] = {

                {"target", "/redfish/v1/Systems/system/LogServices/EventLog/"
                           "Actions/LogService.ClearLog"}};
        });
}

inline void requestRoutesCELogService(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/CELog/")
        // Overwrite normal permissions for CELog
        .privileges({{"ConfigureManager"}})
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
                asyncResp->res.jsonValue["@odata.id"] =
                    "/redfish/v1/Systems/system/LogServices/CELog";
                asyncResp->res.jsonValue["@odata.type"] =
                    "#LogService.v1_1_0.LogService";
                asyncResp->res.jsonValue["Name"] = "CE Log Service";
                asyncResp->res.jsonValue["Description"] =
                    "System CE Log Service";
                asyncResp->res.jsonValue["Id"] = "CELog";
                asyncResp->res.jsonValue["OverWritePolicy"] = "WrapsWhenFull";

                std::pair<std::string, std::string> redfishDateTimeOffset =
                    crow::utility::getDateTimeOffsetNow();

                asyncResp->res.jsonValue["DateTime"] =
                    redfishDateTimeOffset.first;
                asyncResp->res.jsonValue["DateTimeLocalOffset"] =
                    redfishDateTimeOffset.second;

                asyncResp->res.jsonValue["Entries"] = {
                    {"@odata.id", "/redfish/v1/Systems/system/LogServices/"
                                  "CELog/Entries"}};
                asyncResp->res.jsonValue["Actions"]["#LogService.ClearLog"] = {

                    {"target", "/redfish/v1/Systems/system/LogServices/CELog/"
                               "Actions/LogService.ClearLog"}};
            });
}

inline void requestRoutesJournalEventLogClear(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/EventLog/Actions/"
                      "LogService.ClearLog/")
        .privileges(redfish::privileges::
                        postLogServiceSubOverComputerSystemLogServiceCollection)
        .methods(boost::beast::http::verb::post)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
                // Clear the EventLog by deleting the log files
                std::vector<std::filesystem::path> redfishLogFiles;
                if (getRedfishLogFiles(redfishLogFiles))
                {
                    for (const std::filesystem::path& file : redfishLogFiles)
                    {
                        std::error_code ec;
                        std::filesystem::remove(file, ec);
                    }
                }

                // Reload rsyslog so it knows to start new log files
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            BMCWEB_LOG_ERROR << "Failed to reload rsyslog: "
                                             << ec;
                            messages::internalError(asyncResp->res);
                            return;
                        }

                        messages::success(asyncResp->res);
                    },
                    "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
                    "org.freedesktop.systemd1.Manager", "ReloadUnit",
                    "rsyslog.service", "replace");
            });
}

static int fillEventLogEntryJson(const std::string& logEntryID,
                                 const std::string& logEntry,
                                 nlohmann::json& logEntryJson)
{
    // The redfish log format is "<Timestamp> <MessageId>,<MessageArgs>"
    // First get the Timestamp
    size_t space = logEntry.find_first_of(' ');
    if (space == std::string::npos)
    {
        return 1;
    }
    std::string timestamp = logEntry.substr(0, space);
    // Then get the log contents
    size_t entryStart = logEntry.find_first_not_of(' ', space);
    if (entryStart == std::string::npos)
    {
        return 1;
    }
    std::string_view entry(logEntry);
    entry.remove_prefix(entryStart);
    // Use split to separate the entry into its fields
    std::vector<std::string> logEntryFields;
    boost::split(logEntryFields, entry, boost::is_any_of(","),
                 boost::token_compress_on);
    // We need at least a MessageId to be valid
    if (logEntryFields.size() < 1)
    {
        return 1;
    }
    std::string& messageID = logEntryFields[0];

    // Get the Message from the MessageRegistry
    const message_registries::Message* message =
        message_registries::getMessage(messageID);

    std::string msg;
    std::string severity;
    if (message != nullptr)
    {
        msg = message->message;
        severity = message->severity;
    }

    // Get the MessageArgs from the log if there are any
    boost::beast::span<std::string> messageArgs;
    if (logEntryFields.size() > 1)
    {
        std::string& messageArgsStart = logEntryFields[1];
        // If the first string is empty, assume there are no MessageArgs
        std::size_t messageArgsSize = 0;
        if (!messageArgsStart.empty())
        {
            messageArgsSize = logEntryFields.size() - 1;
        }

        messageArgs = {&messageArgsStart, messageArgsSize};

        // Fill the MessageArgs into the Message
        int i = 0;
        for (const std::string& messageArg : messageArgs)
        {
            std::string argStr = "%" + std::to_string(++i);
            size_t argPos = msg.find(argStr);
            if (argPos != std::string::npos)
            {
                msg.replace(argPos, argStr.length(), messageArg);
            }
        }
    }

    // Get the Created time from the timestamp. The log timestamp is in RFC3339
    // format which matches the Redfish format except for the fractional seconds
    // between the '.' and the '+', so just remove them.
    std::size_t dot = timestamp.find_first_of('.');
    std::size_t plus = timestamp.find_first_of('+');
    if (dot != std::string::npos && plus != std::string::npos)
    {
        timestamp.erase(dot, plus - dot);
    }

    // Fill in the log entry with the gathered data
    logEntryJson = {
        {"@odata.type", "#LogEntry.v1_8_0.LogEntry"},
        {"@odata.id",
         "/redfish/v1/Systems/system/LogServices/EventLog/Entries/" +
             logEntryID},
        {"Name", "System Event Log Entry"},
        {"Id", logEntryID},
        {"Message", std::move(msg)},
        {"MessageId", std::move(messageID)},
        {"MessageArgs", messageArgs},
        {"EntryType", "Event"},
        {"Severity", std::move(severity)},
        {"Created", std::move(timestamp)}};
    return 0;
}

inline void requestRoutesJournalEventLogEntryCollection(App& app)
{
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/system/LogServices/EventLog/Entries/")
        .privileges(redfish::privileges::getLogEntryCollection)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request& req,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
                uint64_t skip = 0;
                uint64_t top = maxEntriesPerPage; // Show max entries by default
                if (!getSkipParam(asyncResp, req, skip))
                {
                    return;
                }
                if (!getTopParam(asyncResp, req, top))
                {
                    return;
                }
                // Collections don't include the static data added by SubRoute
                // because it has a duplicate entry for members
                asyncResp->res.jsonValue["@odata.type"] =
                    "#LogEntryCollection.LogEntryCollection";
                asyncResp->res.jsonValue["@odata.id"] =
                    "/redfish/v1/Systems/system/LogServices/EventLog/Entries";
                asyncResp->res.jsonValue["Name"] = "System Event Log Entries";
                asyncResp->res.jsonValue["Description"] =
                    "Collection of System Event Log Entries";

                nlohmann::json& logEntryArray =
                    asyncResp->res.jsonValue["Members"];
                logEntryArray = nlohmann::json::array();
                // Go through the log files and create a unique ID for each
                // entry
                std::vector<std::filesystem::path> redfishLogFiles;
                getRedfishLogFiles(redfishLogFiles);
                uint64_t entryCount = 0;
                std::string logEntry;

                // Oldest logs are in the last file, so start there and loop
                // backwards
                for (auto it = redfishLogFiles.rbegin();
                     it < redfishLogFiles.rend(); it++)
                {
                    std::ifstream logStream(*it);
                    if (!logStream.is_open())
                    {
                        continue;
                    }

                    // Reset the unique ID on the first entry
                    bool firstEntry = true;
                    while (std::getline(logStream, logEntry))
                    {
                        entryCount++;
                        // Handle paging using skip (number of entries to skip
                        // from the start) and top (number of entries to
                        // display)
                        if (entryCount <= skip || entryCount > skip + top)
                        {
                            continue;
                        }

                        std::string idStr;
                        if (!getUniqueEntryID(logEntry, idStr, firstEntry))
                        {
                            continue;
                        }

                        if (firstEntry)
                        {
                            firstEntry = false;
                        }

                        logEntryArray.push_back({});
                        nlohmann::json& bmcLogEntry = logEntryArray.back();
                        if (fillEventLogEntryJson(idStr, logEntry,
                                                  bmcLogEntry) != 0)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                    }
                }
                asyncResp->res.jsonValue["Members@odata.count"] = entryCount;
                if (skip + top < entryCount)
                {
                    asyncResp->res.jsonValue["Members@odata.nextLink"] =
                        "/redfish/v1/Systems/system/LogServices/EventLog/"
                        "Entries?$skip=" +
                        std::to_string(skip + top);
                }
            });
}

inline void requestRoutesJournalEventLogEntry(App& app)
{
    BMCWEB_ROUTE(
        app, "/redfish/v1/Systems/system/LogServices/EventLog/Entries/<str>/")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& param) {
                const std::string& targetID = param;

                // Go through the log files and check the unique ID for each
                // entry to find the target entry
                std::vector<std::filesystem::path> redfishLogFiles;
                getRedfishLogFiles(redfishLogFiles);
                std::string logEntry;

                // Oldest logs are in the last file, so start there and loop
                // backwards
                for (auto it = redfishLogFiles.rbegin();
                     it < redfishLogFiles.rend(); it++)
                {
                    std::ifstream logStream(*it);
                    if (!logStream.is_open())
                    {
                        continue;
                    }

                    // Reset the unique ID on the first entry
                    bool firstEntry = true;
                    while (std::getline(logStream, logEntry))
                    {
                        std::string idStr;
                        if (!getUniqueEntryID(logEntry, idStr, firstEntry))
                        {
                            continue;
                        }

                        if (firstEntry)
                        {
                            firstEntry = false;
                        }

                        if (idStr == targetID)
                        {
                            if (fillEventLogEntryJson(
                                    idStr, logEntry,
                                    asyncResp->res.jsonValue) != 0)
                            {
                                messages::internalError(asyncResp->res);
                                return;
                            }
                            return;
                        }
                    }
                }
                // Requested ID was not found
                messages::resourceMissingAtURI(asyncResp->res, targetID);
            });
}

template <typename Callback>
void getHiddenPropertyValue(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                            const std::string& entryId, Callback&& callback)
{
    auto respHandler = [callback{std::move(callback)},
                        asyncResp](const boost::system::error_code ec,
                                   std::variant<bool>& hiddenProperty) {
        if (ec)
        {
            BMCWEB_LOG_ERROR << "DBUS response error: " << ec;
            messages::internalError(asyncResp->res);
            return;
        }
        bool* hiddenPropValPtr = std::get_if<bool>(&hiddenProperty);
        if (hiddenPropValPtr == nullptr)
        {
            messages::internalError(asyncResp->res);
            return;
        }
        bool hiddenPropVal = *hiddenPropValPtr;
        callback(hiddenPropVal);
    };

    // Get the Hidden Property
    crow::connections::systemBus->async_method_call(
        respHandler, "xyz.openbmc_project.Logging",
        "/xyz/openbmc_project/logging/entry/" + entryId,
        "org.freedesktop.DBus.Properties", "Get",
        "org.open_power.Logging.PEL.Entry", "Hidden");
}

inline void getDBusLogEntryCollection(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    GetManagedObjectsType& resp, eventLogTypes type)
{
    nlohmann::json& entriesArray = asyncResp->res.jsonValue["Members"];
    entriesArray = nlohmann::json::array();
    for (auto& objectPath : resp)
    {
        uint32_t* id = nullptr;
        std::time_t timestamp{};
        std::time_t updateTimestamp{};
        std::string* severity = nullptr;
        std::string* subsystem = nullptr;
        std::string* filePath = nullptr;
        std::string* eventId = nullptr;
        std::string* resolution = nullptr;
        bool resolved = false;
        bool* hiddenProp = nullptr;
        bool serviceProviderNotified = false;
#ifdef BMCWEB_ENABLE_IBM_MANAGEMENT_CONSOLE
        bool managementSystemAck = false;
#endif

        for (auto& interfaceMap : objectPath.second)
        {
            if (interfaceMap.first == "xyz.openbmc_project.Logging.Entry")
            {
                for (auto& propertyMap : interfaceMap.second)
                {
                    if (propertyMap.first == "Id")
                    {
                        id = std::get_if<uint32_t>(&propertyMap.second);
                        if (id == nullptr)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                    }
                    else if (propertyMap.first == "Timestamp")
                    {
                        const uint64_t* millisTimeStamp =
                            std::get_if<uint64_t>(&propertyMap.second);
                        if (millisTimeStamp == nullptr)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        timestamp =
                            crow::utility::getTimestamp(*millisTimeStamp);
                    }
                    else if (propertyMap.first == "UpdateTimestamp")
                    {
                        const uint64_t* millisTimeStamp =
                            std::get_if<uint64_t>(&propertyMap.second);
                        if (millisTimeStamp == nullptr)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        updateTimestamp =
                            crow::utility::getTimestamp(*millisTimeStamp);
                    }
                    else if (propertyMap.first == "Severity")
                    {
                        severity =
                            std::get_if<std::string>(&propertyMap.second);
                        if (severity == nullptr)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                    }
                    else if (propertyMap.first == "Resolution")
                    {
                        resolution =
                            std::get_if<std::string>(&propertyMap.second);
                        if (resolution == nullptr)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                    }
                    else if (propertyMap.first == "EventId")
                    {
                        eventId = std::get_if<std::string>(&propertyMap.second);
                        if (eventId == nullptr)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                    }
                    else if (propertyMap.first == "Resolved")
                    {
                        bool* resolveptr =
                            std::get_if<bool>(&propertyMap.second);
                        if (resolveptr == nullptr)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        resolved = *resolveptr;
                    }
                    else if (propertyMap.first == "ServiceProviderNotify")
                    {
                        bool* serviceProviderNotifiedptr =
                            std::get_if<bool>(&propertyMap.second);
                        if (serviceProviderNotifiedptr == nullptr)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        serviceProviderNotified = *serviceProviderNotifiedptr;
                    }
                }
                if ((id == nullptr) || (resolution == nullptr) ||
                    (severity == nullptr))
                {
                    messages::internalError(asyncResp->res);
                    return;
                }
            }
            else if (interfaceMap.first ==
                     "xyz.openbmc_project.Common.FilePath")
            {
                for (auto& propertyMap : interfaceMap.second)
                {
                    if (propertyMap.first == "Path")
                    {
                        filePath =
                            std::get_if<std::string>(&propertyMap.second);
                    }
                }
            }
            else if (interfaceMap.first == "org.open_power.Logging.PEL.Entry")
            {
                for (auto& propertyMap : interfaceMap.second)
                {
                    if (propertyMap.first == "Hidden")
                    {
                        hiddenProp = std::get_if<bool>(&propertyMap.second);
                        if (hiddenProp == nullptr)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                    }
                    else if (propertyMap.first == "Subsystem")
                    {
                        subsystem =
                            std::get_if<std::string>(&propertyMap.second);
                        if (subsystem == nullptr)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                    }
#ifdef BMCWEB_ENABLE_IBM_MANAGEMENT_CONSOLE
                    else if (propertyMap.first == "ManagementSystemAck")
                    {
                        bool* managementSystemAckptr =
                            std::get_if<bool>(&propertyMap.second);
                        if (managementSystemAckptr == nullptr)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        managementSystemAck = *managementSystemAckptr;
                    }
#endif
                }
            }
        }
        // Object path without the
        // xyz.openbmc_project.Logging.Entry interface and/or
        // org.open_power.Logging.PEL.Entry ignore and continue.
        if ((id == nullptr) || (severity == nullptr) ||
            (hiddenProp == nullptr) || (eventId == nullptr) ||
            (subsystem == nullptr))
        {
            continue;
        }

        std::string entryID = std::to_string(*id);
        // Ignore and continue if the event log entry is 'hidden
        // and EventLog collection' OR 'not hidden and CELog
        // collection'
        if (((type == eventLogTypes::eventLog) && (*hiddenProp)) ||
            ((type == eventLogTypes::ceLog) && !(*hiddenProp)))
        {
            continue;
        }

        entriesArray.push_back({});
        nlohmann::json& thisEntry = entriesArray.back();
        thisEntry["@odata.type"] = "#LogEntry.v1_9_0.LogEntry";
        thisEntry["EntryType"] = "Event";
        thisEntry["Id"] = entryID;
        thisEntry["EventId"] = *eventId;
        thisEntry["Message"] =
            (*eventId).substr(0, 8) + " event in subsystem: " + *subsystem;
        thisEntry["Resolved"] = resolved;
        if (!(*resolution).empty())
        {
            thisEntry["Resolution"] = *resolution;
        }
        thisEntry["ServiceProviderNotified"] = serviceProviderNotified;
        thisEntry["Severity"] = translateSeverityDbusToRedfish(*severity);
        thisEntry["Created"] = crow::utility::getDateTime(timestamp);
        thisEntry["Modified"] = crow::utility::getDateTime(updateTimestamp);
#ifdef BMCWEB_ENABLE_IBM_MANAGEMENT_CONSOLE
        thisEntry["Oem"]["OpenBMC"]["@odata.type"] =
            "#OemLogEntry.v1_0_0.LogEntry";
        thisEntry["Oem"]["OpenBMC"]["ManagementSystemAck"] =
            managementSystemAck;
#endif
        if (type == eventLogTypes::eventLog)
        {
            thisEntry["@odata.id"] = "/redfish/v1/Systems/system/"
                                     "LogServices/EventLog/Entries/" +
                                     entryID;
            thisEntry["Name"] = "System Event Log Entry";

            if (filePath != nullptr)
            {
                thisEntry["AdditionalDataURI"] =
                    "/redfish/v1/Systems/system/LogServices/"
                    "EventLog/Entries/" +
                    entryID + "/attachment";
            }
        }
        else
        {
            thisEntry["@odata.id"] = "/redfish/v1/Systems/system/"
                                     "LogServices/CELog/Entries/" +
                                     entryID;
            thisEntry["Name"] = "System CE Log Entry";

            if (filePath != nullptr)
            {
                thisEntry["AdditionalDataURI"] =
                    "/redfish/v1/Systems/system/LogServices/"
                    "CELog/Entries/" +
                    entryID + "/attachment";
            }
        }
    }
    std::sort(entriesArray.begin(), entriesArray.end(),
              [](const nlohmann::json& left, const nlohmann::json& right) {
                  return (left["Id"] <= right["Id"]);
              });
    asyncResp->res.jsonValue["Members@odata.count"] = entriesArray.size();
}

inline void requestRoutesDBusEventLogEntryCollection(App& app)
{
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/system/LogServices/EventLog/Entries/")
        .privileges(redfish::privileges::getLogEntryCollection)
        .methods(
            boost::beast::http::verb::
                get)([](const crow::Request&,
                        const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
            // Collections don't include the static data added by SubRoute
            // because it has a duplicate entry for members
            asyncResp->res.jsonValue["@odata.type"] =
                "#LogEntryCollection.LogEntryCollection";
            asyncResp->res.jsonValue["@odata.id"] =
                "/redfish/v1/Systems/system/LogServices/EventLog/Entries";
            asyncResp->res.jsonValue["Name"] = "System Event Log Entries";
            asyncResp->res.jsonValue["Description"] =
                "Collection of System Event Log Entries";

            // DBus implementation of EventLog/Entries
            // Make call to Logging Service to find all log entry objects
            crow::connections::systemBus->async_method_call(
                [asyncResp](const boost::system::error_code ec,
                            GetManagedObjectsType& resp) {
                    if (ec)
                    {
                        // TODO Handle for specific error code
                        BMCWEB_LOG_ERROR
                            << "getLogEntriesIfaceData resp_handler got error "
                            << ec;
                        messages::internalError(asyncResp->res);
                        return;
                    }
                    getDBusLogEntryCollection(asyncResp, resp,
                                              eventLogTypes::eventLog);
                },
                "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
                "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
        });
}

inline void requestRoutesDBusCELogEntryCollection(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/CELog/Entries/")
        // Overwrite normal permissions for CELog Entries
        .privileges({{"ConfigureManager"}})
        .methods(
            boost::beast::http::verb::
                get)([](const crow::Request&,
                        const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
            // Collections don't include the static data added by SubRoute
            // because it has a duplicate entry for members
            asyncResp->res.jsonValue["@odata.type"] =
                "#LogEntryCollection.LogEntryCollection";
            asyncResp->res.jsonValue["@odata.id"] =
                "/redfish/v1/Systems/system/LogServices/CELog/"
                "Entries";
            asyncResp->res.jsonValue["Name"] = "System CE Log Entries";
            asyncResp->res.jsonValue["Description"] =
                "Collection of System CE Log Entries";

            // DBus implementation of EventLog/Entries
            // Make call to Logging Service to find all log entry objects
            crow::connections::systemBus->async_method_call(
                [asyncResp](const boost::system::error_code ec,
                            GetManagedObjectsType& resp) {
                    if (ec)
                    {
                        // TODO Handle for specific error code
                        BMCWEB_LOG_ERROR
                            << "getLogEntriesIfaceData resp_handler got error "
                            << ec;
                        messages::internalError(asyncResp->res);
                        return;
                    }
                    getDBusLogEntryCollection(asyncResp, resp,
                                              eventLogTypes::ceLog);
                },
                "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
                "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
        });
}

inline void updateProperty(const crow::Request& req,
                           const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                           const std::string& entryId)
{
    std::optional<bool> resolved;
    std::optional<nlohmann::json> oemObject;
#ifdef BMCWEB_ENABLE_IBM_MANAGEMENT_CONSOLE
    std::optional<bool> managementSystemAck;
#endif
    if (!json_util::readJson(req, asyncResp->res, "Resolved", resolved, "Oem",
                             oemObject))
    {
        return;
    }
    if (resolved.has_value())
    {
        crow::connections::systemBus->async_method_call(
            [asyncResp](const boost::system::error_code ec) {
                if (ec)
                {
                    BMCWEB_LOG_DEBUG << "DBUS response error " << ec;
                    messages::internalError(asyncResp->res);
                    return;
                }
            },
            "xyz.openbmc_project.Logging",
            "/xyz/openbmc_project/logging/entry/" + entryId,
            "org.freedesktop.DBus.Properties", "Set",
            "xyz.openbmc_project.Logging.Entry", "Resolved",
            std::variant<bool>(*resolved));
        BMCWEB_LOG_DEBUG << "Updated Resolved Property";
    }
#ifdef BMCWEB_ENABLE_IBM_MANAGEMENT_CONSOLE
    if (oemObject)
    {
        std::optional<nlohmann::json> bmcOem;
        if (!json_util::readJson(*oemObject, asyncResp->res, "OpenBMC", bmcOem))
        {
            return;
        }
        if (!json_util::readJson(*bmcOem, asyncResp->res, "ManagementSystemAck",
                                 managementSystemAck))
        {
            BMCWEB_LOG_ERROR << "Could not read managementSystemAck";
            return;
        }
    }
    if (managementSystemAck.has_value())
    {
        crow::connections::systemBus->async_method_call(
            [asyncResp](const boost::system::error_code ec) {
                if (ec)
                {
                    BMCWEB_LOG_DEBUG << "DBUS response error " << ec;
                    messages::internalError(asyncResp->res);
                    return;
                }
            },
            "xyz.openbmc_project.Logging",
            "/xyz/openbmc_project/logging/entry/" + entryId,
            "org.freedesktop.DBus.Properties", "Set",
            "org.open_power.Logging.PEL.Entry", "ManagementSystemAck",
            std::variant<bool>(*managementSystemAck));
        BMCWEB_LOG_DEBUG << "Updated ManagementSystemAck Property";
    }
#endif
}

inline void
    deleteEventLogEntry(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                        const std::string& entryId)
{
    // Process response from Logging service.
    auto respHandler = [asyncResp,
                        entryId](const boost::system::error_code ec) {
        BMCWEB_LOG_DEBUG << "EventLogEntry (DBus) doDelete callback: Done";
        if (ec)
        {
            if (ec.value() == EBADR)
            {
                messages::resourceNotFound(asyncResp->res, "LogEntry", entryId);
                return;
            }
            // TODO Handle for specific error code
            BMCWEB_LOG_ERROR << "EventLogEntry (DBus) doDelete "
                                "respHandler got error "
                             << ec;
            asyncResp->res.result(
                boost::beast::http::status::internal_server_error);
            return;
        }

        asyncResp->res.result(boost::beast::http::status::ok);
    };

    // Make call to Logging service to request Delete Log
    crow::connections::systemBus->async_method_call(
        respHandler, "xyz.openbmc_project.Logging",
        "/xyz/openbmc_project/logging/entry/" + entryId,
        "xyz.openbmc_project.Object.Delete", "Delete");
}

inline void getDBusLogEntry(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                            GetManagedPropertyType& resp, eventLogTypes type)
{
    uint32_t* id = nullptr;
    std::time_t timestamp{};
    std::time_t updateTimestamp{};
    std::string* severity = nullptr;
    std::string* filePath = nullptr;
    std::string* eventId = nullptr;
    std::string* subsystem = nullptr;
    std::string* resolution = nullptr;
    bool resolved = false;
    bool* hiddenProp = nullptr;
    bool serviceProviderNotified = false;
#ifdef BMCWEB_ENABLE_IBM_MANAGEMENT_CONSOLE
    bool managementSystemAck = false;
#endif

    for (auto& propertyMap : resp)
    {
        if (propertyMap.first == "Id")
        {
            id = std::get_if<uint32_t>(&propertyMap.second);
            if (id == nullptr)
            {
                messages::internalError(asyncResp->res);
                return;
            }
        }
        else if (propertyMap.first == "Timestamp")
        {
            const uint64_t* millisTimeStamp =
                std::get_if<uint64_t>(&propertyMap.second);
            if (millisTimeStamp == nullptr)
            {
                messages::internalError(asyncResp->res);
                return;
            }
            timestamp = crow::utility::getTimestamp(*millisTimeStamp);
        }
        else if (propertyMap.first == "UpdateTimestamp")
        {
            const uint64_t* millisTimeStamp =
                std::get_if<uint64_t>(&propertyMap.second);
            if (millisTimeStamp == nullptr)
            {
                messages::internalError(asyncResp->res);
                return;
            }
            updateTimestamp = crow::utility::getTimestamp(*millisTimeStamp);
        }
        else if (propertyMap.first == "Severity")
        {
            severity = std::get_if<std::string>(&propertyMap.second);
            if (severity == nullptr)
            {
                messages::internalError(asyncResp->res);
                return;
            }
        }
        else if (propertyMap.first == "EventId")
        {
            eventId = std::get_if<std::string>(&propertyMap.second);
            if (eventId == nullptr)
            {
                messages::internalError(asyncResp->res);
                return;
            }
        }
        else if (propertyMap.first == "Resolution")
        {
            resolution = std::get_if<std::string>(&propertyMap.second);
            if (resolution == nullptr)
            {
                messages::internalError(asyncResp->res);
                return;
            }
        }
        else if (propertyMap.first == "Subsystem")
        {
            subsystem = std::get_if<std::string>(&propertyMap.second);
            if (subsystem == nullptr)
            {
                messages::internalError(asyncResp->res);
                return;
            }
        }
        else if (propertyMap.first == "Resolved")
        {
            bool* resolveptr = std::get_if<bool>(&propertyMap.second);
            if (resolveptr == nullptr)
            {
                messages::internalError(asyncResp->res);
                return;
            }
            resolved = *resolveptr;
        }
        else if (propertyMap.first == "Path")
        {
            filePath = std::get_if<std::string>(&propertyMap.second);
        }
        else if (propertyMap.first == "Hidden")
        {
            hiddenProp = std::get_if<bool>(&propertyMap.second);
            if (hiddenProp == nullptr)
            {
                messages::internalError(asyncResp->res);
                return;
            }
        }
        else if (propertyMap.first == "ServiceProviderNotify")
        {
            bool* serviceProviderNotifiedptr =
                std::get_if<bool>(&propertyMap.second);
            if (serviceProviderNotifiedptr == nullptr)
            {
                messages::internalError(asyncResp->res);
                return;
            }
            serviceProviderNotified = *serviceProviderNotifiedptr;
        }
#ifdef BMCWEB_ENABLE_IBM_MANAGEMENT_CONSOLE
        else if (propertyMap.first == "ManagementSystemAck")
        {
            bool* managementSystemAckptr =
                std::get_if<bool>(&propertyMap.second);
            if (managementSystemAckptr == nullptr)
            {
                messages::internalError(asyncResp->res);
                return;
            }
            managementSystemAck = *managementSystemAckptr;
        }
#endif
    }

    if ((id == nullptr) || (severity == nullptr) || (hiddenProp == nullptr) ||
        (resolution == nullptr) || (eventId == nullptr) ||
        (subsystem == nullptr))
    {
        messages::internalError(asyncResp->res);
        return;
    }

    std::string entryID = std::to_string(*id);

    // Report resource not found if the event log entry is
    // 'hidden and EvenLog collection' OR 'not hidden and CELog
    // collection'
    if (((type == eventLogTypes::eventLog) && (*hiddenProp)) ||
        ((type == eventLogTypes::ceLog) && !(*hiddenProp)))
    {
        messages::resourceNotFound(asyncResp->res, "EventLogEntry", entryID);
        return;
    }

    asyncResp->res.jsonValue["@odata.type"] = "#LogEntry.v1_9_0.LogEntry";
    asyncResp->res.jsonValue["EntryType"] = "Event";
    asyncResp->res.jsonValue["Id"] = entryID;
    asyncResp->res.jsonValue["Message"] =
        (*eventId).substr(0, 8) + " event in subsystem: " + *subsystem;
    asyncResp->res.jsonValue["Resolved"] = resolved;
    asyncResp->res.jsonValue["EventId"] = *eventId;
    if (!(*resolution).empty())
    {
        asyncResp->res.jsonValue["Resolution"] = *resolution;
    }
    asyncResp->res.jsonValue["ServiceProviderNotified"] =
        serviceProviderNotified;
    asyncResp->res.jsonValue["Severity"] =
        translateSeverityDbusToRedfish(*severity);
    asyncResp->res.jsonValue["Created"] = crow::utility::getDateTime(timestamp);
    asyncResp->res.jsonValue["Modified"] =
        crow::utility::getDateTime(updateTimestamp);
#ifdef BMCWEB_ENABLE_IBM_MANAGEMENT_CONSOLE
    asyncResp->res.jsonValue["Oem"]["OpenBMC"]["@odata.type"] =
        "#OemLogEntry.v1_0_0.LogEntry";
    asyncResp->res.jsonValue["Oem"]["OpenBMC"]["ManagementSystemAck"] =
        managementSystemAck;
#endif

    if (type == eventLogTypes::eventLog)
    {
        asyncResp->res.jsonValue["@odata.id"] =
            "/redfish/v1/Systems/system/LogServices/EventLog/"
            "Entries/" +
            entryID;
        asyncResp->res.jsonValue["Name"] = "System Event Log Entry";
        if (filePath != nullptr)
        {
            asyncResp->res.jsonValue["AdditionalDataURI"] =
                "/redfish/v1/Systems/system/LogServices/"
                "EventLog/Entries/" +
                entryID + "/attachment";
        }
    }
    else
    {
        asyncResp->res.jsonValue["@odata.id"] =
            "/redfish/v1/Systems/system/LogServices/CELog/"
            "Entries/" +
            entryID;
        asyncResp->res.jsonValue["Name"] = "System CE Log Entry";
        if (filePath != nullptr)
        {
            asyncResp->res.jsonValue["AdditionalDataURI"] =
                "/redfish/v1/Systems/system/LogServices/"
                "CELog/Entries/" +
                entryID + "/attachment";
        }
    }
}

inline void requestRoutesDBusEventLogEntry(App& app)
{
    BMCWEB_ROUTE(
        app, "/redfish/v1/Systems/system/LogServices/EventLog/Entries/<str>/")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& param)

            {
                std::string entryID = param;
                dbus::utility::escapePathForDbus(entryID);

                // DBus implementation of EventLog/Entries
                // Make call to Logging Service to find all log entry objects
                crow::connections::systemBus->async_method_call(
                    [asyncResp, entryID](const boost::system::error_code ec,
                                         GetManagedPropertyType& resp) {
                        if (ec.value() == EBADR)
                        {
                            messages::resourceNotFound(
                                asyncResp->res, "EventLogEntry", entryID);
                            return;
                        }
                        if (ec)
                        {
                            BMCWEB_LOG_ERROR << "EventLogEntry (DBus) "
                                                "resp_handler got error "
                                             << ec;
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        getDBusLogEntry(asyncResp, resp,
                                        eventLogTypes::eventLog);
                    },
                    "xyz.openbmc_project.Logging",
                    "/xyz/openbmc_project/logging/entry/" + entryID,
                    "org.freedesktop.DBus.Properties", "GetAll", "");
            });

    BMCWEB_ROUTE(
        app, "/redfish/v1/Systems/system/LogServices/EventLog/Entries/<str>/")
        .privileges(redfish::privileges::patchLogEntry)
        .methods(boost::beast::http::verb::patch)(
            [](const crow::Request& req,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& entryId) {
                auto updatePropertyCallback = [req, asyncResp,
                                               entryId](bool hiddenPropVal) {
                    if (hiddenPropVal)
                    {
                        messages::resourceNotFound(asyncResp->res, "LogEntry",
                                                   entryId);
                        return;
                    }
                    updateProperty(req, asyncResp, entryId);
                };
                getHiddenPropertyValue(asyncResp, entryId,
                                       std::move(updatePropertyCallback));
            });

    BMCWEB_ROUTE(
        app, "/redfish/v1/Systems/system/LogServices/EventLog/Entries/<str>/")
        .privileges(redfish::privileges::deleteLogEntry)
        .methods(boost::beast::http::verb::delete_)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& param) {
                BMCWEB_LOG_DEBUG << "Do delete single event entries.";
                std::string entryID = param;
                dbus::utility::escapePathForDbus(entryID);

                auto deleteEventLogCallback = [asyncResp,
                                               entryID](bool hiddenPropVal) {
                    if (hiddenPropVal)
                    {
                        messages::resourceNotFound(asyncResp->res, "LogEntry",
                                                   entryID);
                        return;
                    }
                    deleteEventLogEntry(asyncResp, entryID);
                };
                getHiddenPropertyValue(asyncResp, entryID,
                                       std::move(deleteEventLogCallback));
            });
}

inline void requestRoutesDBusCELogEntry(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/"
                      "CELog/Entries/<str>/")
        // Overwrite normal permissions for CELog Entry
        .privileges({{"ConfigureManager"}})
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& param)

            {
                std::string entryID = param;
                dbus::utility::escapePathForDbus(entryID);

                // DBus implementation of EventLog/Entries
                // Make call to Logging Service to find all log entry objects
                crow::connections::systemBus->async_method_call(
                    [asyncResp, entryID](const boost::system::error_code ec,
                                         GetManagedPropertyType& resp) {
                        if (ec.value() == EBADR)
                        {
                            messages::resourceNotFound(
                                asyncResp->res, "EventLogEntry", entryID);
                            return;
                        }
                        if (ec)
                        {
                            BMCWEB_LOG_ERROR << "EventLogEntry (DBus) "
                                                "resp_handler got error "
                                             << ec;
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        getDBusLogEntry(asyncResp, resp, eventLogTypes::ceLog);
                    },
                    "xyz.openbmc_project.Logging",
                    "/xyz/openbmc_project/logging/entry/" + entryID,
                    "org.freedesktop.DBus.Properties", "GetAll", "");
            });

    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/"
                      "CELog/Entries/<str>/")
        .privileges(redfish::privileges::patchLogEntry)
        .methods(boost::beast::http::verb::patch)(
            [](const crow::Request& req,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& entryId) {
                auto updatePropertyCallback = [req, asyncResp,
                                               entryId](bool hiddenPropVal) {
                    if (!hiddenPropVal)
                    {
                        messages::resourceNotFound(asyncResp->res, "LogEntry",
                                                   entryId);
                        return;
                    }
                    updateProperty(req, asyncResp, entryId);
                };
                getHiddenPropertyValue(asyncResp, entryId,
                                       std::move(updatePropertyCallback));
            });

    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/"
                      "CELog/Entries/<str>/")
        .privileges(redfish::privileges::deleteLogEntry)
        .methods(boost::beast::http::verb::delete_)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& param) {
                BMCWEB_LOG_DEBUG << "Do delete single event entries.";

                std::string entryID = param;
                dbus::utility::escapePathForDbus(entryID);

                auto deleteEventLogCallabck = [asyncResp,
                                               entryID](bool hiddenPropVal) {
                    if (!hiddenPropVal)
                    {
                        messages::resourceNotFound(asyncResp->res, "LogEntry",
                                                   entryID);
                        return;
                    }
                    deleteEventLogEntry(asyncResp, entryID);
                };

                getHiddenPropertyValue(asyncResp, entryID,
                                       std::move(deleteEventLogCallabck));
            });
}

inline void
    displayOemPelAttachment(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                            const std::string& entryID)
{

    auto respHandler = [asyncResp, entryID](const boost::system::error_code ec,
                                            const std::string& pelJson) {
        if (ec.value() == EBADR)
        {
            messages::resourceNotFound(asyncResp->res, "OemPelAttachment",
                                       entryID);
            return;
        }
        if (ec)
        {
            BMCWEB_LOG_DEBUG << "DBUS response error " << ec;
            messages::internalError(asyncResp->res);
            return;
        }

        asyncResp->res.jsonValue["Oem"]["IBM"]["PelJson"] = pelJson;
        asyncResp->res.jsonValue["Oem"]["@odata.type"] =
            "#OemLogEntryAttachment.Oem";
        asyncResp->res.jsonValue["Oem"]["IBM"]["@odata.type"] =
            "#OemLogEntryAttachment.IBM";
    };

    uint32_t id;
    auto [ptrIndex, ecIndex] =
        std::from_chars(entryID.data(), entryID.data() + entryID.size(), id);

    if (ecIndex != std::errc())
    {
        BMCWEB_LOG_DEBUG << "Unable to convert to entryID " << entryID
                         << " to uint32_t";
        messages::internalError(asyncResp->res);
        return;
    }

    crow::connections::systemBus->async_method_call(
        respHandler, "xyz.openbmc_project.Logging",
        "/xyz/openbmc_project/logging", "org.open_power.Logging.PEL",
        "GetPELJSON", id);
}

inline void requestRoutesDBusEventLogEntryDownloadPelJson(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/EventLog/Entries/"
                      "<str>/OemPelAttachment")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& param)

            {
                std::string entryID = param;
                dbus::utility::escapePathForDbus(entryID);

                auto eventLogAttachmentCallback =
                    [asyncResp, entryID](bool hiddenPropVal) {
                        if (hiddenPropVal)
                        {
                            messages::resourceNotFound(asyncResp->res,
                                                       "LogEntry", entryID);
                            return;
                        }
                        displayOemPelAttachment(asyncResp, entryID);
                    };
                getHiddenPropertyValue(asyncResp, entryID,
                                       std::move(eventLogAttachmentCallback));
            });
}

inline void requestRoutesDBusCELogEntryDownloadPelJson(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/CELog/Entries/"
                      "<str>/OemPelAttachment")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& param)

            {
                std::string entryID = param;
                dbus::utility::escapePathForDbus(entryID);

                auto eventLogAttachmentCallback =
                    [asyncResp, entryID](bool hiddenPropVal) {
                        if (!hiddenPropVal)
                        {
                            messages::resourceNotFound(asyncResp->res,
                                                       "LogEntry", entryID);
                            return;
                        }
                        displayOemPelAttachment(asyncResp, entryID);
                    };
                getHiddenPropertyValue(asyncResp, entryID,
                                       std::move(eventLogAttachmentCallback));
            });
}

inline void getEventLogEntryAttachment(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& entryID)
{
    auto respHandler = [asyncResp,
                        entryID](const boost::system::error_code ec,
                                 const sdbusplus::message::unix_fd& unixfd) {
        if (ec.value() == EBADR)
        {
            messages::resourceNotFound(asyncResp->res, "CELogAttachment",
                                       entryID);
            return;
        }
        if (ec)
        {
            BMCWEB_LOG_DEBUG << "DBUS response error " << ec;
            messages::internalError(asyncResp->res);
            return;
        }

        int fd = -1;
        fd = dup(unixfd);
        if (fd == -1)
        {
            messages::internalError(asyncResp->res);
            return;
        }

        long long int size = lseek(fd, 0, SEEK_END);
        if (size == -1)
        {
            messages::internalError(asyncResp->res);
            return;
        }

        // Arbitrary max size of 64kb
        constexpr int maxFileSize = 65536;
        if (size > maxFileSize)
        {
            BMCWEB_LOG_ERROR << "File size exceeds maximum allowed size of "
                             << maxFileSize;
            messages::internalError(asyncResp->res);
            return;
        }
        std::vector<char> data(static_cast<size_t>(size));
        long long int rc = lseek(fd, 0, SEEK_SET);
        if (rc == -1)
        {
            messages::internalError(asyncResp->res);
            return;
        }
        rc = read(fd, data.data(), data.size());
        if ((rc == -1) || (rc != size))
        {
            messages::internalError(asyncResp->res);
            return;
        }
        close(fd);

        std::string_view strData(data.data(), data.size());
        std::string output = crow::utility::base64encode(strData);

        asyncResp->res.addHeader("Content-Type", "application/octet-stream");
        asyncResp->res.addHeader("Content-Transfer-Encoding", "Base64");
        asyncResp->res.body() = std::move(output);
    };

    crow::connections::systemBus->async_method_call(
        respHandler, "xyz.openbmc_project.Logging",
        "/xyz/openbmc_project/logging/entry/" + entryID,
        "xyz.openbmc_project.Logging.Entry", "GetEntry");
}

inline void requestRoutesDBusEventLogEntryDownload(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/EventLog/Entries/"
                      "<str>/attachment")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request& req,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& param)

            {
                if (!http_helpers::isOctetAccepted(
                        req.getHeaderValue("Accept")))
                {
                    asyncResp->res.result(
                        boost::beast::http::status::bad_request);
                    return;
                }

                std::string entryID = param;
                dbus::utility::escapePathForDbus(entryID);

                auto eventLogAttachmentCallback =
                    [asyncResp, entryID](bool hiddenPropVal) {
                        if (hiddenPropVal)
                        {
                            messages::resourceNotFound(asyncResp->res,
                                                       "LogEntry", entryID);
                            return;
                        }
                        getEventLogEntryAttachment(asyncResp, entryID);
                    };
                getHiddenPropertyValue(asyncResp, entryID,
                                       std::move(eventLogAttachmentCallback));
            });
}

inline void requestRoutesDBusCELogEntryDownload(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/CELog/Entries/"
                      "<str>/attachment")
        // Overwrite normal permissions for LogEntry attachment
        .privileges({{"ConfigureManager"}})
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request& req,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& param)

            {
                std::string_view acceptHeader = req.getHeaderValue("Accept");
                // The iterators in boost/http/rfc7230.hpp end the string if '/'
                // is found, so replace it with arbitrary character '|' which is
                // not part of the Accept header syntax.
                std::string acceptStr = boost::replace_all_copy(
                    std::string(acceptHeader), "/", "|");
                boost::beast::http::ext_list acceptTypes{acceptStr};
                bool supported = false;
                for (const auto& type : acceptTypes)
                {
                    if ((type.first == "*|*") ||
                        (type.first == "application|octet-stream"))
                    {
                        supported = true;
                        break;
                    }
                }
                if (!supported)
                {
                    asyncResp->res.result(
                        boost::beast::http::status::bad_request);
                    return;
                }

                std::string entryID = param;
                dbus::utility::escapePathForDbus(entryID);

                auto eventLogAttachmentCallback =
                    [asyncResp, entryID](bool hiddenPropVal) {
                        if (!hiddenPropVal)
                        {
                            messages::resourceNotFound(asyncResp->res,
                                                       "LogEntry", entryID);
                            return;
                        }
                        getEventLogEntryAttachment(asyncResp, entryID);
                    };

                getHiddenPropertyValue(asyncResp, entryID,
                                       std::move(eventLogAttachmentCallback));
            });
}

inline void requestRoutesBMCLogServiceCollection(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Managers/bmc/LogServices/")
        .privileges(redfish::privileges::getLogServiceCollection)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
                // Collections don't include the static data added by SubRoute
                // because it has a duplicate entry for members
                asyncResp->res.jsonValue["@odata.type"] =
                    "#LogServiceCollection.LogServiceCollection";
                asyncResp->res.jsonValue["@odata.id"] =
                    "/redfish/v1/Managers/bmc/LogServices";
                asyncResp->res.jsonValue["Name"] =
                    "Open BMC Log Services Collection";
                asyncResp->res.jsonValue["Description"] =
                    "Collection of LogServices for this Manager";
                nlohmann::json& logServiceArray =
                    asyncResp->res.jsonValue["Members"];
                logServiceArray = nlohmann::json::array();
#ifdef BMCWEB_ENABLE_REDFISH_DUMP_LOG
                logServiceArray.push_back(
                    {{"@odata.id",
                      "/redfish/v1/Managers/bmc/LogServices/Dump"}});
#endif
#ifdef BMCWEB_ENABLE_REDFISH_BMC_JOURNAL
                logServiceArray.push_back(
                    {{"@odata.id",
                      "/redfish/v1/Managers/bmc/LogServices/Journal"}});
#endif
                asyncResp->res.jsonValue["Members@odata.count"] =
                    logServiceArray.size();
            });
}

inline void requestRoutesBMCJournalLogService(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Managers/bmc/LogServices/Journal/")
        .privileges(redfish::privileges::getLogService)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)

            {
                asyncResp->res.jsonValue["@odata.type"] =
                    "#LogService.v1_1_0.LogService";
                asyncResp->res.jsonValue["@odata.id"] =
                    "/redfish/v1/Managers/bmc/LogServices/Journal";
                asyncResp->res.jsonValue["Name"] =
                    "Open BMC Journal Log Service";
                asyncResp->res.jsonValue["Description"] =
                    "BMC Journal Log Service";
                asyncResp->res.jsonValue["Id"] = "Journal";
                asyncResp->res.jsonValue["OverWritePolicy"] = "WrapsWhenFull";

                std::pair<std::string, std::string> redfishDateTimeOffset =
                    crow::utility::getDateTimeOffsetNow();
                asyncResp->res.jsonValue["DateTime"] =
                    redfishDateTimeOffset.first;
                asyncResp->res.jsonValue["DateTimeLocalOffset"] =
                    redfishDateTimeOffset.second;

                asyncResp->res.jsonValue["Entries"] = {
                    {"@odata.id",
                     "/redfish/v1/Managers/bmc/LogServices/Journal/Entries"}};
            });
}

static int fillBMCJournalLogEntryJson(const std::string& bmcJournalLogEntryID,
                                      sd_journal* journal,
                                      nlohmann::json& bmcJournalLogEntryJson)
{
    // Get the Log Entry contents
    int ret = 0;

    std::string message;
    std::string_view syslogID;
    ret = getJournalMetadata(journal, "SYSLOG_IDENTIFIER", syslogID);
    if (ret < 0)
    {
        BMCWEB_LOG_ERROR << "Failed to read SYSLOG_IDENTIFIER field: "
                         << strerror(-ret);
    }
    if (!syslogID.empty())
    {
        message += std::string(syslogID) + ": ";
    }

    std::string_view msg;
    ret = getJournalMetadata(journal, "MESSAGE", msg);
    if (ret < 0)
    {
        BMCWEB_LOG_ERROR << "Failed to read MESSAGE field: " << strerror(-ret);
        return 1;
    }
    message += std::string(msg);

    // Get the severity from the PRIORITY field
    long int severity = 8; // Default to an invalid priority
    ret = getJournalMetadata(journal, "PRIORITY", 10, severity);
    if (ret < 0)
    {
        BMCWEB_LOG_ERROR << "Failed to read PRIORITY field: " << strerror(-ret);
    }

    // Get the Created time from the timestamp
    std::string entryTimeStr;
    if (!getEntryTimestamp(journal, entryTimeStr))
    {
        return 1;
    }

    // Fill in the log entry with the gathered data
    bmcJournalLogEntryJson = {
        {"@odata.type", "#LogEntry.v1_8_0.LogEntry"},
        {"@odata.id", "/redfish/v1/Managers/bmc/LogServices/Journal/Entries/" +
                          bmcJournalLogEntryID},
        {"Name", "BMC Journal Entry"},
        {"Id", bmcJournalLogEntryID},
        {"Message", std::move(message)},
        {"EntryType", "Oem"},
        {"Severity", severity <= 2   ? "Critical"
                     : severity <= 4 ? "Warning"
                                     : "OK"},
        {"OemRecordFormat", "BMC Journal Entry"},
        {"Created", std::move(entryTimeStr)}};
    return 0;
}

inline void requestRoutesBMCJournalLogEntryCollection(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Managers/bmc/LogServices/Journal/Entries/")
        .privileges(redfish::privileges::getLogEntryCollection)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request& req,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
                static constexpr const long maxEntriesPerPage = 1000;
                uint64_t skip = 0;
                uint64_t top = maxEntriesPerPage; // Show max entries by default
                if (!getSkipParam(asyncResp, req, skip))
                {
                    return;
                }
                if (!getTopParam(asyncResp, req, top))
                {
                    return;
                }
                // Collections don't include the static data added by SubRoute
                // because it has a duplicate entry for members
                asyncResp->res.jsonValue["@odata.type"] =
                    "#LogEntryCollection.LogEntryCollection";
                asyncResp->res.jsonValue["@odata.id"] =
                    "/redfish/v1/Managers/bmc/LogServices/Journal/Entries";
                asyncResp->res.jsonValue["Name"] = "Open BMC Journal Entries";
                asyncResp->res.jsonValue["Description"] =
                    "Collection of BMC Journal Entries";
                nlohmann::json& logEntryArray =
                    asyncResp->res.jsonValue["Members"];
                logEntryArray = nlohmann::json::array();

                // Go through the journal and use the timestamp to create a
                // unique ID for each entry
                sd_journal* journalTmp = nullptr;
                int ret = sd_journal_open(&journalTmp, SD_JOURNAL_LOCAL_ONLY);
                if (ret < 0)
                {
                    BMCWEB_LOG_ERROR << "failed to open journal: "
                                     << strerror(-ret);
                    messages::internalError(asyncResp->res);
                    return;
                }
                std::unique_ptr<sd_journal, decltype(&sd_journal_close)>
                    journal(journalTmp, sd_journal_close);
                journalTmp = nullptr;
                uint64_t entryCount = 0;
                // Reset the unique ID on the first entry
                bool firstEntry = true;
                SD_JOURNAL_FOREACH(journal.get())
                {
                    entryCount++;
                    // Handle paging using skip (number of entries to skip from
                    // the start) and top (number of entries to display)
                    if (entryCount <= skip || entryCount > skip + top)
                    {
                        continue;
                    }

                    std::string idStr;
                    if (!getUniqueEntryID(journal.get(), idStr, firstEntry))
                    {
                        continue;
                    }

                    if (firstEntry)
                    {
                        firstEntry = false;
                    }

                    logEntryArray.push_back({});
                    nlohmann::json& bmcJournalLogEntry = logEntryArray.back();
                    if (fillBMCJournalLogEntryJson(idStr, journal.get(),
                                                   bmcJournalLogEntry) != 0)
                    {
                        messages::internalError(asyncResp->res);
                        return;
                    }
                }
                asyncResp->res.jsonValue["Members@odata.count"] = entryCount;
                if (skip + top < entryCount)
                {
                    asyncResp->res.jsonValue["Members@odata.nextLink"] =
                        "/redfish/v1/Managers/bmc/LogServices/Journal/"
                        "Entries?$skip=" +
                        std::to_string(skip + top);
                }
            });
}

inline void requestRoutesBMCJournalLogEntry(App& app)
{
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Managers/bmc/LogServices/Journal/Entries/<str>/")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& entryID) {
                // Convert the unique ID back to a timestamp to find the entry
                uint64_t ts = 0;
                uint64_t index = 0;
                if (!getTimestampFromID(asyncResp, entryID, ts, index))
                {
                    return;
                }

                sd_journal* journalTmp = nullptr;
                int ret = sd_journal_open(&journalTmp, SD_JOURNAL_LOCAL_ONLY);
                if (ret < 0)
                {
                    BMCWEB_LOG_ERROR << "failed to open journal: "
                                     << strerror(-ret);
                    messages::internalError(asyncResp->res);
                    return;
                }
                std::unique_ptr<sd_journal, decltype(&sd_journal_close)>
                    journal(journalTmp, sd_journal_close);
                journalTmp = nullptr;
                // Go to the timestamp in the log and move to the entry at the
                // index tracking the unique ID
                std::string idStr;
                bool firstEntry = true;
                ret = sd_journal_seek_realtime_usec(journal.get(), ts);
                if (ret < 0)
                {
                    BMCWEB_LOG_ERROR << "failed to seek to an entry in journal"
                                     << strerror(-ret);
                    messages::internalError(asyncResp->res);
                    return;
                }
                for (uint64_t i = 0; i <= index; i++)
                {
                    sd_journal_next(journal.get());
                    if (!getUniqueEntryID(journal.get(), idStr, firstEntry))
                    {
                        messages::internalError(asyncResp->res);
                        return;
                    }
                    if (firstEntry)
                    {
                        firstEntry = false;
                    }
                }
                // Confirm that the entry ID matches what was requested
                if (idStr != entryID)
                {
                    messages::resourceMissingAtURI(asyncResp->res, entryID);
                    return;
                }

                if (fillBMCJournalLogEntryJson(entryID, journal.get(),
                                               asyncResp->res.jsonValue) != 0)
                {
                    messages::internalError(asyncResp->res);
                    return;
                }
            });
}

inline void requestRoutesBMCDumpService(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Managers/bmc/LogServices/Dump/")
        .privileges(redfish::privileges::getLogService)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
                asyncResp->res.jsonValue["@odata.id"] =
                    "/redfish/v1/Managers/bmc/LogServices/Dump";
                asyncResp->res.jsonValue["@odata.type"] =
                    "#LogService.v1_2_0.LogService";
                asyncResp->res.jsonValue["Name"] = "Dump LogService";
                asyncResp->res.jsonValue["Description"] = "BMC Dump LogService";
                asyncResp->res.jsonValue["Id"] = "Dump";
                asyncResp->res.jsonValue["OverWritePolicy"] = "WrapsWhenFull";

                std::pair<std::string, std::string> redfishDateTimeOffset =
                    crow::utility::getDateTimeOffsetNow();
                asyncResp->res.jsonValue["DateTime"] =
                    redfishDateTimeOffset.first;
                asyncResp->res.jsonValue["DateTimeLocalOffset"] =
                    redfishDateTimeOffset.second;

                asyncResp->res.jsonValue["Entries"] = {
                    {"@odata.id",
                     "/redfish/v1/Managers/bmc/LogServices/Dump/Entries"}};
                asyncResp->res.jsonValue["Actions"] = {
                    {"#LogService.ClearLog",
                     {{"target", "/redfish/v1/Managers/bmc/LogServices/Dump/"
                                 "Actions/LogService.ClearLog"}}},
                    {"#LogService.CollectDiagnosticData",
                     {{"target", "/redfish/v1/Managers/bmc/LogServices/Dump/"
                                 "Actions/LogService.CollectDiagnosticData"}}}};
            });
}

inline void requestRoutesBMCDumpEntryCollection(App& app)
{

    /**
     * Functions triggers appropriate requests on DBus
     */
    BMCWEB_ROUTE(app, "/redfish/v1/Managers/bmc/LogServices/Dump/Entries/")
        .privileges(redfish::privileges::getLogEntryCollection)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
                asyncResp->res.jsonValue["@odata.type"] =
                    "#LogEntryCollection.LogEntryCollection";
                asyncResp->res.jsonValue["@odata.id"] =
                    "/redfish/v1/Managers/bmc/LogServices/Dump/Entries";
                asyncResp->res.jsonValue["Name"] = "BMC Dump Entries";
                asyncResp->res.jsonValue["Description"] =
                    "Collection of BMC Dump Entries";

                getDumpEntryCollection(asyncResp, "BMC");
            });
}

inline void requestRoutesBMCDumpEntry(App& app)
{
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Managers/bmc/LogServices/Dump/Entries/<str>/")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& param) {
                getDumpEntryById(asyncResp, param, "BMC");
            });
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Managers/bmc/LogServices/Dump/Entries/<str>/")
        .privileges(redfish::privileges::deleteLogEntry)
        .methods(boost::beast::http::verb::delete_)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& param) {
                deleteDumpEntry(asyncResp, param, "bmc");
            });
}

inline void requestRoutesBMCDumpCreate(App& app)
{

    BMCWEB_ROUTE(app, "/redfish/v1/Managers/bmc/LogServices/Dump/"
                      "Actions/"
                      "LogService.CollectDiagnosticData/")
        .privileges(redfish::privileges::postLogService)
        .methods(boost::beast::http::verb::post)(
            [](const crow::Request& req,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
                createDump(asyncResp, req, "BMC");
            });
}

inline void requestRoutesBMCDumpClear(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Managers/bmc/LogServices/Dump/"
                      "Actions/"
                      "LogService.ClearLog/")
        .privileges(redfish::privileges::postLogService)
        .methods(boost::beast::http::verb::post)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
                clearDump(asyncResp, "BMC");
            });
}

inline void requestRoutesSystemDumpService(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/Dump/")
        .privileges(redfish::privileges::getLogService)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)

            {
                asyncResp->res.jsonValue["@odata.id"] =
                    "/redfish/v1/Systems/system/LogServices/Dump";
                asyncResp->res.jsonValue["@odata.type"] =
                    "#LogService.v1_2_0.LogService";
                asyncResp->res.jsonValue["Name"] = "Dump LogService";
                asyncResp->res.jsonValue["Description"] =
                    "System Dump LogService";
                asyncResp->res.jsonValue["Id"] = "Dump";
                asyncResp->res.jsonValue["OverWritePolicy"] = "WrapsWhenFull";

                std::pair<std::string, std::string> redfishDateTimeOffset =
                    crow::utility::getDateTimeOffsetNow();
                asyncResp->res.jsonValue["DateTime"] =
                    redfishDateTimeOffset.first;
                asyncResp->res.jsonValue["DateTimeLocalOffset"] =
                    redfishDateTimeOffset.second;

                asyncResp->res.jsonValue["Entries"] = {
                    {"@odata.id",
                     "/redfish/v1/Systems/system/LogServices/Dump/Entries"}};
                asyncResp->res.jsonValue["Actions"] = {
                    {"#LogService.ClearLog",
                     {{"target",
                       "/redfish/v1/Systems/system/LogServices/Dump/Actions/"
                       "LogService.ClearLog"}}},
                    {"#LogService.CollectDiagnosticData",
                     {{"target",
                       "/redfish/v1/Systems/system/LogServices/Dump/Actions/"
                       "LogService.CollectDiagnosticData"}}}};
            });
}

inline void requestRoutesSystemDumpEntryCollection(App& app)
{

    /**
     * Functions triggers appropriate requests on DBus
     */
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/Dump/Entries/")
        .privileges(redfish::privileges::getLogEntryCollection)
        .methods(
            boost::beast::http::verb::
                get)([](const crow::Request&,
                        const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
            asyncResp->res.jsonValue["@odata.type"] =
                "#LogEntryCollection.LogEntryCollection";
            asyncResp->res.jsonValue["@odata.id"] =
                "/redfish/v1/Systems/system/LogServices/Dump/Entries";
            asyncResp->res.jsonValue["Name"] = "System Dump Entries";
            asyncResp->res.jsonValue["Description"] =
                "Collection of System, Resource, Hostboot, Hardware & SBE Dump "
                "Entries";

            getDumpEntryCollection(asyncResp, "System");
            getDumpEntryCollection(asyncResp, "Resource");
            getDumpEntryCollection(asyncResp, "Hostboot");
            getDumpEntryCollection(asyncResp, "Hardware");
            getDumpEntryCollection(asyncResp, "SBE");
        });
}

inline void requestRoutesSystemDumpEntry(App& app)
{
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/system/LogServices/Dump/Entries/<str>/")
        .privileges(redfish::privileges::getLogEntry)

        .methods(boost::beast::http::verb::get)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& param) {
                if (boost::starts_with(param, "System"))
                {
                    getDumpEntryById(asyncResp, param, "System");
                }
                else if (boost::starts_with(param, "Resource"))
                {
                    getDumpEntryById(asyncResp, param, "Resource");
                }
                else if (boost::starts_with(param, "Hostboot"))
                {
                    getDumpEntryById(asyncResp, param, "Hostboot");
                }
                else if (boost::starts_with(param, "Hardware"))
                {
                    getDumpEntryById(asyncResp, param, "Hardware");
                }
                else if (boost::starts_with(param, "SBE"))
                {
                    getDumpEntryById(asyncResp, param, "SBE");
                }
                else
                {
                    messages::invalidObject(asyncResp->res, "Dump Id");
                    return;
                }
            });

    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/system/LogServices/Dump/Entries/<str>/")
        .privileges(redfish::privileges::deleteLogEntry)
        .methods(boost::beast::http::verb::delete_)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& param) {
                std::size_t pos = param.find_first_of('_');
                if (pos == std::string::npos || (pos + 1) >= param.length())
                {
                    // Requested ID is invalid
                    messages::invalidObject(asyncResp->res, "Dump Id");
                    return;
                }

                const std::string& dumpId = param.substr(pos + 1);
                if (boost::starts_with(param, "System"))
                {
                    deleteDumpEntry(asyncResp, dumpId, "system");
                }
                else if (boost::starts_with(param, "Resource"))
                {
                    deleteDumpEntry(asyncResp, dumpId, "resource");
                }
                else if (boost::starts_with(param, "Hostboot"))
                {
                    deleteDumpEntry(asyncResp, dumpId, "hostboot");
                }
                else if (boost::starts_with(param, "Hardware"))
                {
                    deleteDumpEntry(asyncResp, dumpId, "hardware");
                }
                else if (boost::starts_with(param, "SBE"))
                {
                    deleteDumpEntry(asyncResp, dumpId, "SBE");
                }
                else
                {
                    messages::invalidObject(asyncResp->res, "Dump Id");
                    return;
                }
            });
}

inline void requestRoutesSystemDumpCreate(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/Dump/"
                      "Actions/"
                      "LogService.CollectDiagnosticData/")
        .privileges(redfish::privileges::postLogService)
        .methods(boost::beast::http::verb::post)(
            [](const crow::Request& req,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)

            { createDump(asyncResp, req, "System"); });
}

inline void requestRoutesSystemDumpClear(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/Dump/"
                      "Actions/"
                      "LogService.ClearLog/")
        .privileges(redfish::privileges::postLogService)
        .methods(boost::beast::http::verb::post)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)

            {
                clearDump(asyncResp, "System");
                clearDump(asyncResp, "Resource");
                clearDump(asyncResp, "Hostboot");
                clearDump(asyncResp, "Hardware");
                clearDump(asyncResp, "SBE");
            });
}

inline void requestRoutesCrashdumpService(App& app)
{
    // Note: Deviated from redfish privilege registry for GET & HEAD
    // method for security reasons.
    /**
     * Functions triggers appropriate requests on DBus
     */
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/Crashdump/")
        .privileges(redfish::privileges::getLogService)
        .methods(
            boost::beast::http::verb::
                get)([](const crow::Request&,
                        const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
            // Copy over the static data to include the entries added by
            // SubRoute
            asyncResp->res.jsonValue["@odata.id"] =
                "/redfish/v1/Systems/system/LogServices/Crashdump";
            asyncResp->res.jsonValue["@odata.type"] =
                "#LogService.v1_2_0.LogService";
            asyncResp->res.jsonValue["Name"] = "Open BMC Oem Crashdump Service";
            asyncResp->res.jsonValue["Description"] = "Oem Crashdump Service";
            asyncResp->res.jsonValue["Id"] = "Oem Crashdump";
            asyncResp->res.jsonValue["OverWritePolicy"] = "WrapsWhenFull";
            asyncResp->res.jsonValue["MaxNumberOfRecords"] = 3;

            std::pair<std::string, std::string> redfishDateTimeOffset =
                crow::utility::getDateTimeOffsetNow();
            asyncResp->res.jsonValue["DateTime"] = redfishDateTimeOffset.first;
            asyncResp->res.jsonValue["DateTimeLocalOffset"] =
                redfishDateTimeOffset.second;

            asyncResp->res.jsonValue["Entries"] = {
                {"@odata.id",
                 "/redfish/v1/Systems/system/LogServices/Crashdump/Entries"}};
            asyncResp->res.jsonValue["Actions"] = {
                {"#LogService.ClearLog",
                 {{"target", "/redfish/v1/Systems/system/LogServices/Crashdump/"
                             "Actions/LogService.ClearLog"}}},
                {"#LogService.CollectDiagnosticData",
                 {{"target", "/redfish/v1/Systems/system/LogServices/Crashdump/"
                             "Actions/LogService.CollectDiagnosticData"}}}};
        });
}

void inline requestRoutesCrashdumpClear(App& app)
{
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/system/LogServices/Crashdump/Actions/"
                 "LogService.ClearLog/")
        .privileges(redfish::privileges::
                        postLogServiceSubOverComputerSystemLogServiceCollection)
        .methods(boost::beast::http::verb::post)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec,
                                const std::string&) {
                        if (ec)
                        {
                            messages::internalError(asyncResp->res);
                            return;
                        }
                        messages::success(asyncResp->res);
                    },
                    crashdumpObject, crashdumpPath, deleteAllInterface,
                    "DeleteAll");
            });
}

static void
    logCrashdumpEntry(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                      const std::string& logID, nlohmann::json& logEntryJson)
{
    auto getStoredLogCallback =
        [asyncResp, logID, &logEntryJson](
            const boost::system::error_code ec,
            const std::vector<std::pair<std::string, VariantType>>& params) {
            if (ec)
            {
                BMCWEB_LOG_DEBUG << "failed to get log ec: " << ec.message();
                if (ec.value() ==
                    boost::system::linux_error::bad_request_descriptor)
                {
                    messages::resourceNotFound(asyncResp->res, "LogEntry",
                                               logID);
                }
                else
                {
                    messages::internalError(asyncResp->res);
                }
                return;
            }

            std::string timestamp{};
            std::string filename{};
            std::string logfile{};
            parseCrashdumpParameters(params, filename, timestamp, logfile);

            if (filename.empty() || timestamp.empty())
            {
                messages::resourceMissingAtURI(asyncResp->res, logID);
                return;
            }

            std::string crashdumpURI =
                "/redfish/v1/Systems/system/LogServices/Crashdump/Entries/" +
                logID + "/" + filename;
            logEntryJson = {{"@odata.type", "#LogEntry.v1_7_0.LogEntry"},
                            {"@odata.id", "/redfish/v1/Systems/system/"
                                          "LogServices/Crashdump/Entries/" +
                                              logID},
                            {"Name", "CPU Crashdump"},
                            {"Id", logID},
                            {"EntryType", "Oem"},
                            {"AdditionalDataURI", std::move(crashdumpURI)},
                            {"DiagnosticDataType", "OEM"},
                            {"OEMDiagnosticDataType", "PECICrashdump"},
                            {"Created", std::move(timestamp)}};
        };
    crow::connections::systemBus->async_method_call(
        std::move(getStoredLogCallback), crashdumpObject,
        crashdumpPath + std::string("/") + logID,
        "org.freedesktop.DBus.Properties", "GetAll", crashdumpInterface);
}

inline void requestRoutesCrashdumpEntryCollection(App& app)
{
    // Note: Deviated from redfish privilege registry for GET & HEAD
    // method for security reasons.
    /**
     * Functions triggers appropriate requests on DBus
     */
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/system/LogServices/Crashdump/Entries/")
        .privileges(redfish::privileges::getLogEntryCollection)
        .methods(
            boost::beast::http::verb::
                get)([](const crow::Request&,
                        const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
            // Collections don't include the static data added by SubRoute
            // because it has a duplicate entry for members
            auto getLogEntriesCallback = [asyncResp](
                                             const boost::system::error_code ec,
                                             const std::vector<std::string>&
                                                 resp) {
                if (ec)
                {
                    if (ec.value() !=
                        boost::system::errc::no_such_file_or_directory)
                    {
                        BMCWEB_LOG_DEBUG << "failed to get entries ec: "
                                         << ec.message();
                        messages::internalError(asyncResp->res);
                        return;
                    }
                }
                asyncResp->res.jsonValue["@odata.type"] =
                    "#LogEntryCollection.LogEntryCollection";
                asyncResp->res.jsonValue["@odata.id"] =
                    "/redfish/v1/Systems/system/LogServices/Crashdump/Entries";
                asyncResp->res.jsonValue["Name"] = "Open BMC Crashdump Entries";
                asyncResp->res.jsonValue["Description"] =
                    "Collection of Crashdump Entries";
                nlohmann::json& logEntryArray =
                    asyncResp->res.jsonValue["Members"];
                logEntryArray = nlohmann::json::array();
                std::vector<std::string> logIDs;
                // Get the list of log entries and build up an empty array big
                // enough to hold them
                for (const std::string& objpath : resp)
                {
                    // Get the log ID
                    std::size_t lastPos = objpath.rfind('/');
                    if (lastPos == std::string::npos)
                    {
                        continue;
                    }
                    logIDs.emplace_back(objpath.substr(lastPos + 1));

                    // Add a space for the log entry to the array
                    logEntryArray.push_back({});
                }
                // Now go through and set up async calls to fill in the entries
                size_t index = 0;
                for (const std::string& logID : logIDs)
                {
                    // Add the log entry to the array
                    logCrashdumpEntry(asyncResp, logID, logEntryArray[index++]);
                }
                asyncResp->res.jsonValue["Members@odata.count"] =
                    logEntryArray.size();
            };
            crow::connections::systemBus->async_method_call(
                std::move(getLogEntriesCallback),
                "xyz.openbmc_project.ObjectMapper",
                "/xyz/openbmc_project/object_mapper",
                "xyz.openbmc_project.ObjectMapper", "GetSubTreePaths", "", 0,
                std::array<const char*, 1>{crashdumpInterface});
        });
}

inline void requestRoutesCrashdumpEntry(App& app)
{
    // Note: Deviated from redfish privilege registry for GET & HEAD
    // method for security reasons.

    BMCWEB_ROUTE(
        app, "/redfish/v1/Systems/system/LogServices/Crashdump/Entries/<str>/")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& param) {
                const std::string& logID = param;
                logCrashdumpEntry(asyncResp, logID, asyncResp->res.jsonValue);
            });
}

inline void requestRoutesCrashdumpFile(App& app)
{
    // Note: Deviated from redfish privilege registry for GET & HEAD
    // method for security reasons.
    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Systems/system/LogServices/Crashdump/Entries/<str>/<str>/")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& logID, const std::string& fileName) {
                auto getStoredLogCallback =
                    [asyncResp, logID, fileName](
                        const boost::system::error_code ec,
                        const std::vector<std::pair<std::string, VariantType>>&
                            resp) {
                        if (ec)
                        {
                            BMCWEB_LOG_DEBUG << "failed to get log ec: "
                                             << ec.message();
                            messages::internalError(asyncResp->res);
                            return;
                        }

                        std::string dbusFilename{};
                        std::string dbusTimestamp{};
                        std::string dbusFilepath{};

                        parseCrashdumpParameters(resp, dbusFilename,
                                                 dbusTimestamp, dbusFilepath);

                        if (dbusFilename.empty() || dbusTimestamp.empty() ||
                            dbusFilepath.empty())
                        {
                            messages::resourceMissingAtURI(asyncResp->res,
                                                           fileName);
                            return;
                        }

                        // Verify the file name parameter is correct
                        if (fileName != dbusFilename)
                        {
                            messages::resourceMissingAtURI(asyncResp->res,
                                                           fileName);
                            return;
                        }

                        if (!std::filesystem::exists(dbusFilepath))
                        {
                            messages::resourceMissingAtURI(asyncResp->res,
                                                           fileName);
                            return;
                        }
                        std::ifstream ifs(dbusFilepath, std::ios::in |
                                                            std::ios::binary |
                                                            std::ios::ate);
                        std::ifstream::pos_type fileSize = ifs.tellg();
                        if (fileSize < 0)
                        {
                            messages::generalError(asyncResp->res);
                            return;
                        }
                        ifs.seekg(0, std::ios::beg);

                        auto crashData = std::make_unique<char[]>(
                            static_cast<unsigned int>(fileSize));

                        ifs.read(crashData.get(), static_cast<int>(fileSize));

                        // The cast to std::string is intentional in order to
                        // use the assign() that applies move mechanics
                        asyncResp->res.body().assign(
                            static_cast<std::string>(crashData.get()));

                        // Configure this to be a file download when accessed
                        // from a browser
                        asyncResp->res.addHeader("Content-Disposition",
                                                 "attachment");
                    };
                crow::connections::systemBus->async_method_call(
                    std::move(getStoredLogCallback), crashdumpObject,
                    crashdumpPath + std::string("/") + logID,
                    "org.freedesktop.DBus.Properties", "GetAll",
                    crashdumpInterface);
            });
}

inline void requestRoutesCrashdumpCollect(App& app)
{
    // Note: Deviated from redfish privilege registry for GET & HEAD
    // method for security reasons.
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/Crashdump/"
                      "Actions/LogService.CollectDiagnosticData/")
        .privileges(redfish::privileges::
                        postLogServiceSubOverComputerSystemLogServiceCollection)
        .methods(
            boost::beast::http::verb::
                post)([](const crow::Request& req,
                         const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
            std::string diagnosticDataType;
            std::string oemDiagnosticDataType;
            if (!redfish::json_util::readJson(
                    req, asyncResp->res, "DiagnosticDataType",
                    diagnosticDataType, "OEMDiagnosticDataType",
                    oemDiagnosticDataType))
            {
                return;
            }

            if (diagnosticDataType != "OEM")
            {
                BMCWEB_LOG_ERROR
                    << "Only OEM DiagnosticDataType supported for Crashdump";
                messages::actionParameterValueFormatError(
                    asyncResp->res, diagnosticDataType, "DiagnosticDataType",
                    "CollectDiagnosticData");
                return;
            }

            auto collectCrashdumpCallback = [asyncResp, req](
                                                const boost::system::error_code
                                                    ec,
                                                const std::string&) {
                if (ec)
                {
                    if (ec.value() ==
                        boost::system::errc::operation_not_supported)
                    {
                        messages::resourceInStandby(asyncResp->res);
                    }
                    else if (ec.value() ==
                             boost::system::errc::device_or_resource_busy)
                    {
                        messages::serviceTemporarilyUnavailable(asyncResp->res,
                                                                "60");
                    }
                    else
                    {
                        messages::internalError(asyncResp->res);
                    }
                    return;
                }
                std::shared_ptr<task::TaskData> task =
                    task::TaskData::createTask(
                        [](boost::system::error_code err,
                           sdbusplus::message::message&,
                           const std::shared_ptr<task::TaskData>& taskData) {
                            if (!err)
                            {
                                taskData->messages.emplace_back(
                                    messages::taskCompletedOK(
                                        std::to_string(taskData->index)));
                                taskData->state = "Completed";
                            }
                            return task::completed;
                        },
                        "type='signal',interface='org.freedesktop.DBus."
                        "Properties',"
                        "member='PropertiesChanged',arg0namespace='com.intel."
                        "crashdump'");
                task->startTimer(std::chrono::minutes(5));
                task->populateResp(asyncResp->res);
                task->payload.emplace(req);
            };

            if (oemDiagnosticDataType == "OnDemand")
            {
                crow::connections::systemBus->async_method_call(
                    std::move(collectCrashdumpCallback), crashdumpObject,
                    crashdumpPath, crashdumpOnDemandInterface,
                    "GenerateOnDemandLog");
            }
            else if (oemDiagnosticDataType == "Telemetry")
            {
                crow::connections::systemBus->async_method_call(
                    std::move(collectCrashdumpCallback), crashdumpObject,
                    crashdumpPath, crashdumpTelemetryInterface,
                    "GenerateTelemetryLog");
            }
            else
            {
                BMCWEB_LOG_ERROR << "Unsupported OEMDiagnosticDataType: "
                                 << oemDiagnosticDataType;
                messages::actionParameterValueFormatError(
                    asyncResp->res, oemDiagnosticDataType,
                    "OEMDiagnosticDataType", "CollectDiagnosticData");
                return;
            }
        });
}

/**
 * DBusLogServiceActionsClear class supports POST method for ClearLog action.
 */
inline void requestRoutesDBusLogServiceActionsClear(App& app)
{
    /**
     * Function handles POST method request.
     * The Clear Log actions does not require any parameter.The action deletes
     * all entries found in the Entries collection for this Log Service.
     */

    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/EventLog/Actions/"
                      "LogService.ClearLog/")
        .privileges(redfish::privileges::postLogService)
        .methods(boost::beast::http::verb::post)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
                BMCWEB_LOG_DEBUG << "Do delete all entries.";

                // Process response from Logging service.
                auto respHandler = [asyncResp](
                                       const boost::system::error_code ec) {
                    BMCWEB_LOG_DEBUG
                        << "doClearLog resp_handler callback: Done";
                    if (ec)
                    {
                        // TODO Handle for specific error code
                        BMCWEB_LOG_ERROR << "doClearLog resp_handler got error "
                                         << ec;
                        asyncResp->res.result(
                            boost::beast::http::status::internal_server_error);
                        return;
                    }

                    asyncResp->res.result(
                        boost::beast::http::status::no_content);
                };

                // Make call to Logging service to request Clear Log
                crow::connections::systemBus->async_method_call(
                    respHandler, "xyz.openbmc_project.Logging",
                    "/xyz/openbmc_project/logging",
                    "xyz.openbmc_project.Collection.DeleteAll", "DeleteAll");
            });
}

/**
 * DBusCollectableLogServiceActionsClear class supports POST method for ClearLog
 * action.
 */
inline void requestRoutesDBusCELogServiceActionsClear(App& app)
{
    /**
     * Function handles POST method request.
     * The Clear Log actions does not require any parameter.The action deletes
     * all entries found in the Entries collection for this Log Service
     * irrespective of 'Hidden' property value
     */

    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/CELog/Actions/"
                      "LogService.ClearLog/")
        .privileges(redfish::privileges::postLogService)
        .methods(boost::beast::http::verb::post)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
                BMCWEB_LOG_DEBUG << "Do delete all entries.";

                // Process response from Logging service.
                auto respHandler = [asyncResp](
                                       const boost::system::error_code ec) {
                    BMCWEB_LOG_DEBUG
                        << "doClearLog resp_handler callback: Done";
                    if (ec)
                    {
                        // TODO Handle for specific error code
                        BMCWEB_LOG_ERROR << "doClearLog resp_handler got error "
                                         << ec;
                        asyncResp->res.result(
                            boost::beast::http::status::internal_server_error);
                        return;
                    }

                    asyncResp->res.result(
                        boost::beast::http::status::no_content);
                };

                // Make call to Logging service to request Clear Log
                crow::connections::systemBus->async_method_call(
                    respHandler, "xyz.openbmc_project.Logging",
                    "/xyz/openbmc_project/logging",
                    "xyz.openbmc_project.Collection.DeleteAll", "DeleteAll");
            });
}

/****************************************************
 * Redfish PostCode interfaces
 * using DBUS interface: getPostCodesTS
 ******************************************************/
inline void requestRoutesPostCodesLogService(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/PostCodes/")
        .privileges(redfish::privileges::getLogService)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
                asyncResp->res.jsonValue = {
                    {"@odata.id",
                     "/redfish/v1/Systems/system/LogServices/PostCodes"},
                    {"@odata.type", "#LogService.v1_1_0.LogService"},
                    {"Name", "POST Code Log Service"},
                    {"Description", "POST Code Log Service"},
                    {"Id", "PostCodes"},
                    {"OverWritePolicy", "WrapsWhenFull"},
                    {"Entries",
                     {{"@odata.id", "/redfish/v1/Systems/system/LogServices/"
                                    "PostCodes/Entries"}}}};

                std::pair<std::string, std::string> redfishDateTimeOffset =
                    crow::utility::getDateTimeOffsetNow();
                asyncResp->res.jsonValue["DateTime"] =
                    redfishDateTimeOffset.first;
                asyncResp->res.jsonValue["DateTimeLocalOffset"] =
                    redfishDateTimeOffset.second;

                asyncResp->res.jsonValue["Actions"]["#LogService.ClearLog"] = {
                    {"target",
                     "/redfish/v1/Systems/system/LogServices/PostCodes/"
                     "Actions/LogService.ClearLog"}};
            });
}

inline void requestRoutesPostCodesClear(App& app)
{
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/system/LogServices/PostCodes/Actions/"
                 "LogService.ClearLog/")
        .privileges(redfish::privileges::
                        postLogServiceSubOverComputerSystemLogServiceCollection)
        .methods(boost::beast::http::verb::post)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
                BMCWEB_LOG_DEBUG << "Do delete all postcodes entries.";

                // Make call to post-code service to request clear all
                crow::connections::systemBus->async_method_call(
                    [asyncResp](const boost::system::error_code ec) {
                        if (ec)
                        {
                            // TODO Handle for specific error code
                            BMCWEB_LOG_ERROR
                                << "doClearPostCodes resp_handler got error "
                                << ec;
                            asyncResp->res.result(boost::beast::http::status::
                                                      internal_server_error);
                            messages::internalError(asyncResp->res);
                            return;
                        }
                    },
                    "xyz.openbmc_project.State.Boot.PostCode0",
                    "/xyz/openbmc_project/State/Boot/PostCode0",
                    "xyz.openbmc_project.Collection.DeleteAll", "DeleteAll");
            });
}

static void fillPostCodeEntry(
    const std::shared_ptr<bmcweb::AsyncResp>& aResp,
    const boost::container::flat_map<
        uint64_t, std::tuple<uint64_t, std::vector<uint8_t>>>& postcode,
    const uint16_t bootIndex, const uint64_t codeIndex = 0,
    const uint64_t skip = 0, const uint64_t top = 0)
{
    // Get the Message from the MessageRegistry
    const message_registries::Message* message =
        message_registries::getMessage("OpenBMC.0.2.BIOSPOSTCodeASCII");

    uint64_t currentCodeIndex = 0;
    nlohmann::json& logEntryArray = aResp->res.jsonValue["Members"];

    uint64_t firstCodeTimeUs = 0;
    for (const std::pair<uint64_t, std::tuple<uint64_t, std::vector<uint8_t>>>&
             code : postcode)
    {
        currentCodeIndex++;
        std::string postcodeEntryID =
            "B" + std::to_string(bootIndex) + "-" +
            std::to_string(currentCodeIndex); // 1 based index in EntryID string

        uint64_t usecSinceEpoch = code.first;
        uint64_t usTimeOffset = 0;

        if (1 == currentCodeIndex)
        { // already incremented
            firstCodeTimeUs = code.first;
        }
        else
        {
            usTimeOffset = code.first - firstCodeTimeUs;
        }

        // skip if no specific codeIndex is specified and currentCodeIndex does
        // not fall between top and skip
        if ((codeIndex == 0) &&
            (currentCodeIndex <= skip || currentCodeIndex > top))
        {
            continue;
        }

        // skip if a specific codeIndex is specified and does not match the
        // currentIndex
        if ((codeIndex > 0) && (currentCodeIndex != codeIndex))
        {
            // This is done for simplicity. 1st entry is needed to calculate
            // time offset. To improve efficiency, one can get to the entry
            // directly (possibly with flatmap's nth method)
            continue;
        }

        // currentCodeIndex is within top and skip or equal to specified code
        // index

        // Get the Created time from the timestamp
        std::string entryTimeStr;
        entryTimeStr = crow::utility::getDateTime(
            static_cast<std::time_t>(usecSinceEpoch / 1000 / 1000));

        // assemble messageArgs: BootIndex, TimeOffset(100us), PostCode(hex)
        std::ostringstream hexCode;
        hexCode << "0x" << std::setfill('0') << std::setw(2) << std::hex
                << std::get<0>(code.second);
        std::string stringCode =
            crow::utility::convertToAscii(std::get<uint64_t>(code.second));
        std::ostringstream timeOffsetStr;
        // Set Fixed -Point Notation
        timeOffsetStr << std::fixed;
        // Set precision to 4 digits
        timeOffsetStr << std::setprecision(4);
        // Add double to stream
        timeOffsetStr << static_cast<double>(usTimeOffset) / 1000 / 1000;
        std::vector<std::string> messageArgs = {std::to_string(bootIndex),
                                                timeOffsetStr.str(),
                                                hexCode.str(), stringCode};

        // Get MessageArgs template from message registry
        std::string msg;
        if (message != nullptr)
        {
            msg = message->message;

            // fill in this post code value
            int i = 0;
            for (const std::string& messageArg : messageArgs)
            {
                std::string argStr = "%" + std::to_string(++i);
                size_t argPos = msg.find(argStr);
                if (argPos != std::string::npos)
                {
                    msg.replace(argPos, argStr.length(), messageArg);
                }
            }
        }

        // Get Severity template from message registry
        std::string severity;
        if (message != nullptr)
        {
            severity = message->severity;
        }

        // add to AsyncResp
        logEntryArray.push_back({});
        nlohmann::json& bmcLogEntry = logEntryArray.back();
        bmcLogEntry = {{"@odata.type", "#LogEntry.v1_8_0.LogEntry"},
                       {"@odata.id", "/redfish/v1/Systems/system/LogServices/"
                                     "PostCodes/Entries/" +
                                         postcodeEntryID},
                       {"Name", "POST Code Log Entry"},
                       {"Id", postcodeEntryID},
                       {"Message", std::move(msg)},
                       {"MessageId", "OpenBMC.0.2.BIOSPOSTCodeASCII"},
                       {"MessageArgs", std::move(messageArgs)},
                       {"EntryType", "Event"},
                       {"Severity", std::move(severity)},
                       {"Created", entryTimeStr}};
        if (!std::get<std::vector<uint8_t>>(code.second).empty())
        {
            bmcLogEntry["AdditionalDataURI"] =
                "/redfish/v1/Systems/system/LogServices/PostCodes/Entries/" +
                postcodeEntryID + "/attachment";
        }
    }
}

static void getPostCodeForEntry(const std::shared_ptr<bmcweb::AsyncResp>& aResp,
                                const uint16_t bootIndex,
                                const uint64_t codeIndex)
{
    crow::connections::systemBus->async_method_call(
        [aResp, bootIndex,
         codeIndex](const boost::system::error_code ec,
                    const boost::container::flat_map<
                        uint64_t, std::tuple<uint64_t, std::vector<uint8_t>>>&
                        postcode) {
            if (ec)
            {
                BMCWEB_LOG_DEBUG << "DBUS POST CODE PostCode response error";
                messages::internalError(aResp->res);
                return;
            }

            // skip the empty postcode boots
            if (postcode.empty())
            {
                return;
            }

            fillPostCodeEntry(aResp, postcode, bootIndex, codeIndex);

            aResp->res.jsonValue["Members@odata.count"] =
                aResp->res.jsonValue["Members"].size();
        },
        "xyz.openbmc_project.State.Boot.PostCode0",
        "/xyz/openbmc_project/State/Boot/PostCode0",
        "xyz.openbmc_project.State.Boot.PostCode", "GetPostCodesWithTimeStamp",
        bootIndex);
}

static void getPostCodeForBoot(const std::shared_ptr<bmcweb::AsyncResp>& aResp,
                               const uint16_t bootIndex,
                               const uint16_t bootCount,
                               const uint64_t entryCount, const uint64_t skip,
                               const uint64_t top)
{
    crow::connections::systemBus->async_method_call(
        [aResp, bootIndex, bootCount, entryCount, skip,
         top](const boost::system::error_code ec,
              const boost::container::flat_map<
                  uint64_t, std::tuple<uint64_t, std::vector<uint8_t>>>&
                  postcode) {
            if (ec)
            {
                BMCWEB_LOG_DEBUG << "DBUS POST CODE PostCode response error";
                messages::internalError(aResp->res);
                return;
            }

            uint64_t endCount = entryCount;
            if (!postcode.empty())
            {
                endCount = entryCount + postcode.size();

                if ((skip < endCount) && ((top + skip) > entryCount))
                {
                    uint64_t thisBootSkip =
                        std::max(skip, entryCount) - entryCount;
                    uint64_t thisBootTop =
                        std::min(top + skip, endCount) - entryCount;

                    fillPostCodeEntry(aResp, postcode, bootIndex, 0,
                                      thisBootSkip, thisBootTop);
                }
                aResp->res.jsonValue["Members@odata.count"] = endCount;
            }

            // continue to previous bootIndex
            if (bootIndex < bootCount)
            {
                getPostCodeForBoot(aResp, static_cast<uint16_t>(bootIndex + 1),
                                   bootCount, endCount, skip, top);
            }
            else
            {
                aResp->res.jsonValue["Members@odata.nextLink"] =
                    "/redfish/v1/Systems/system/LogServices/PostCodes/"
                    "Entries?$skip=" +
                    std::to_string(skip + top);
            }
        },
        "xyz.openbmc_project.State.Boot.PostCode0",
        "/xyz/openbmc_project/State/Boot/PostCode0",
        "xyz.openbmc_project.State.Boot.PostCode", "GetPostCodesWithTimeStamp",
        bootIndex);
}

static void
    getCurrentBootNumber(const std::shared_ptr<bmcweb::AsyncResp>& aResp,
                         const uint64_t skip, const uint64_t top)
{
    uint64_t entryCount = 0;
    crow::connections::systemBus->async_method_call(
        [aResp, entryCount, skip,
         top](const boost::system::error_code ec,
              const std::variant<uint16_t>& bootCount) {
            if (ec)
            {
                BMCWEB_LOG_DEBUG << "DBUS response error " << ec;
                messages::internalError(aResp->res);
                return;
            }
            auto pVal = std::get_if<uint16_t>(&bootCount);
            if (pVal)
            {
                getPostCodeForBoot(aResp, 1, *pVal, entryCount, skip, top);
            }
            else
            {
                BMCWEB_LOG_DEBUG << "Post code boot index failed.";
            }
        },
        "xyz.openbmc_project.State.Boot.PostCode0",
        "/xyz/openbmc_project/State/Boot/PostCode0",
        "org.freedesktop.DBus.Properties", "Get",
        "xyz.openbmc_project.State.Boot.PostCode", "CurrentBootCycleCount");
}

inline void requestRoutesPostCodesEntryCollection(App& app)
{
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/system/LogServices/PostCodes/Entries/")
        .privileges(redfish::privileges::getLogEntryCollection)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request& req,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp) {
                asyncResp->res.jsonValue["@odata.type"] =
                    "#LogEntryCollection.LogEntryCollection";
                asyncResp->res.jsonValue["@odata.id"] =
                    "/redfish/v1/Systems/system/LogServices/PostCodes/Entries";
                asyncResp->res.jsonValue["Name"] = "BIOS POST Code Log Entries";
                asyncResp->res.jsonValue["Description"] =
                    "Collection of POST Code Log Entries";
                asyncResp->res.jsonValue["Members"] = nlohmann::json::array();
                asyncResp->res.jsonValue["Members@odata.count"] = 0;

                uint64_t skip = 0;
                uint64_t top = maxEntriesPerPage; // Show max entries by default
                if (!getSkipParam(asyncResp, req, skip))
                {
                    return;
                }
                if (!getTopParam(asyncResp, req, top))
                {
                    return;
                }
                getCurrentBootNumber(asyncResp, skip, top);
            });
}

/**
 * @brief Parse post code ID and get the current value and index value
 *        eg: postCodeID=B1-2, currentValue=1, index=2
 *
 * @param[in]  postCodeID     Post Code ID
 * @param[out] currentValue   Current value
 * @param[out] index          Index value
 *
 * @return bool true if the parsing is successful, false the parsing fails
 */
inline static bool parsePostCode(const std::string& postCodeID,
                                 uint64_t& currentValue, uint16_t& index)
{
    std::vector<std::string> split;
    boost::algorithm::split(split, postCodeID, boost::is_any_of("-"));
    if (split.size() != 2 || split[0].length() < 2 || split[0].front() != 'B')
    {
        return false;
    }

    const char* start = split[0].data() + 1;
    const char* end = split[0].data() + split[0].size();
    auto [ptrIndex, ecIndex] = std::from_chars(start, end, index);

    if (ptrIndex != end || ecIndex != std::errc())
    {
        return false;
    }

    start = split[1].data();
    end = split[1].data() + split[1].size();
    auto [ptrValue, ecValue] = std::from_chars(start, end, currentValue);
    if (ptrValue != end || ecValue != std::errc())
    {
        return false;
    }

    return true;
}

inline void requestRoutesPostCodesEntryAdditionalData(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/PostCodes/"
                      "Entries/<str>/attachment/")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request& req,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& postCodeID) {
                if (!http_helpers::isOctetAccepted(
                        req.getHeaderValue("Accept")))
                {
                    asyncResp->res.result(
                        boost::beast::http::status::bad_request);
                    return;
                }

                uint64_t currentValue = 0;
                uint16_t index = 0;
                if (!parsePostCode(postCodeID, currentValue, index))
                {
                    messages::resourceNotFound(asyncResp->res, "LogEntry",
                                               postCodeID);
                    return;
                }

                crow::connections::systemBus->async_method_call(
                    [asyncResp, postCodeID, currentValue](
                        const boost::system::error_code ec,
                        const std::vector<std::tuple<
                            uint64_t, std::vector<uint8_t>>>& postcodes) {
                        if (ec.value() == EBADR)
                        {
                            messages::resourceNotFound(asyncResp->res,
                                                       "LogEntry", postCodeID);
                            return;
                        }
                        if (ec)
                        {
                            BMCWEB_LOG_DEBUG << "DBUS response error " << ec;
                            messages::internalError(asyncResp->res);
                            return;
                        }

                        size_t value = static_cast<size_t>(currentValue) - 1;
                        if (value == std::string::npos ||
                            postcodes.size() < currentValue)
                        {
                            BMCWEB_LOG_ERROR << "Wrong currentValue value";
                            messages::resourceNotFound(asyncResp->res,
                                                       "LogEntry", postCodeID);
                            return;
                        }

                        auto& [tID, code] = postcodes[value];
                        if (code.empty())
                        {
                            BMCWEB_LOG_INFO << "No found post code data";
                            messages::resourceNotFound(asyncResp->res,
                                                       "LogEntry", postCodeID);
                            return;
                        }

                        std::string_view strData(
                            reinterpret_cast<const char*>(code.data()),
                            code.size());

                        asyncResp->res.addHeader("Content-Type",
                                                 "application/octet-stream");
                        asyncResp->res.addHeader("Content-Transfer-Encoding",
                                                 "Base64");
                        asyncResp->res.body() =
                            crow::utility::base64encode(strData);
                    },
                    "xyz.openbmc_project.State.Boot.PostCode0",
                    "/xyz/openbmc_project/State/Boot/PostCode0",
                    "xyz.openbmc_project.State.Boot.PostCode", "GetPostCodes",
                    index);
            });
}

inline void requestRoutesPostCodesEntry(App& app)
{
    BMCWEB_ROUTE(
        app, "/redfish/v1/Systems/system/LogServices/PostCodes/Entries/<str>/")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(
            [](const crow::Request&,
               const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
               const std::string& targetID) {
                uint16_t bootIndex = 0;
                uint64_t codeIndex = 0;
                if (!parsePostCode(targetID, codeIndex, bootIndex))
                {
                    // Requested ID was not found
                    messages::resourceMissingAtURI(asyncResp->res, targetID);
                    return;
                }
                if (bootIndex == 0 || codeIndex == 0)
                {
                    BMCWEB_LOG_DEBUG << "Get Post Code invalid entry string "
                                     << targetID;
                }

                asyncResp->res.jsonValue["@odata.type"] =
                    "#LogEntry.v1_4_0.LogEntry";
                asyncResp->res.jsonValue["@odata.id"] =
                    "/redfish/v1/Systems/system/LogServices/PostCodes/"
                    "Entries";
                asyncResp->res.jsonValue["Name"] = "BIOS POST Code Log Entries";
                asyncResp->res.jsonValue["Description"] =
                    "Collection of POST Code Log Entries";
                asyncResp->res.jsonValue["Members"] = nlohmann::json::array();
                asyncResp->res.jsonValue["Members@odata.count"] = 0;

                getPostCodeForEntry(asyncResp, bootIndex, codeIndex);
            });
}

#ifdef BMCWEB_ENABLE_HW_ISOLATION
/**
 * @brief API Used to add the supported HardwareIsolation LogServices Members
 *
 * @param[in] req - The HardwareIsolation redfish request (unused now).
 * @param[in] asyncResp - The redfish response to return.
 *
 * @return The redfish response in the given buffer.
 */
inline void getSystemHardwareIsolationLogService(
    const crow::Request& /* req */,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    asyncResp->res.jsonValue["@odata.id"] =
        "/redfish/v1/Systems/system/LogServices/"
        "HardwareIsolation";
    asyncResp->res.jsonValue["@odata.type"] = "#LogService.v1_2_0.LogService";
    asyncResp->res.jsonValue["Name"] = "Hardware Isolation LogService";
    asyncResp->res.jsonValue["Description"] =
        "Hardware Isolation LogService for system owned "
        "devices";
    asyncResp->res.jsonValue["Id"] = "HardwareIsolation";

    asyncResp->res.jsonValue["Entries"] = {
        {"@odata.id", "/redfish/v1/Systems/system/LogServices/"
                      "HardwareIsolation/Entries"}};

    asyncResp->res.jsonValue["Actions"] = {
        {"#LogService.ClearLog",
         {{"target", "/redfish/v1/Systems/system/LogServices/"
                     "HardwareIsolation/Actions/"
                     "LogService.ClearLog"}}}};
}

/**
 * @brief Workaround to handle DCM (Dual-Chip Module) package for Redfish
 *
 * This API will make sure processor modeled as dual chip module, If yes then,
 * replace the redfish processor id as "dcmN-cpuN" because redfish currently
 * does not support chip module concept.
 *
 * @param[in] dbusObjPath - The D-Bus object path to return the object instance
 *
 * @return the object instance with it parent instance id if the given object
 *         is a processor else the object instance alone.
 */
inline std::string
    getIsolatedHwItemId(const sdbusplus::message::object_path& dbusObjPath)
{
    std::string isolatedHwItemId;

    if ((dbusObjPath.filename().find("cpu") != std::string::npos) &&
        (dbusObjPath.parent_path().filename().find("dcm") != std::string::npos))
    {
        isolatedHwItemId = std::string(dbusObjPath.parent_path().filename() +
                                       "-" + dbusObjPath.filename());
    }
    else
    {
        isolatedHwItemId = dbusObjPath.filename();
    }
    return isolatedHwItemId;
}

/**
 * @brief API used to get redfish uri of the given dbus object and fill into
 *        "OriginOfCondition" property of LogEntry schema.
 *
 * @param[in] asyncResp - The redfish response to return.
 * @param[in] dbusObjPath - The DBus object path which represents redfishUri.
 * @param[in] entryJsonIdx - The json entry index to add isolated hardware
 *                            details in the appropriate entry json object.
 *
 * @return The redfish response with "OriginOfCondition" property of
 *         LogEntry schema if success else return the error
 */
inline void getRedfishUriByDbusObjPath(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const sdbusplus::message::object_path& dbusObjPath,
    const size_t entryJsonIdx)
{
    crow::connections::systemBus->async_method_call(
        [asyncResp, dbusObjPath, entryJsonIdx](
            const boost::system::error_code ec, const GetObjectType& objType) {
            if (ec || objType.empty())
            {
                BMCWEB_LOG_ERROR << "DBUS response error [" << ec.value()
                                 << " : " << ec.message()
                                 << "] when tried to get the RedfishURI of "
                                 << "isolated hareware: " << dbusObjPath.str;
                messages::internalError(asyncResp->res);
                return;
            }

            RedfishUriListType::const_iterator redfishUriIt;
            for (const auto& service : objType)
            {
                for (const auto& interface : service.second)
                {
                    redfishUriIt = redfishUriList.find(interface);
                    if (redfishUriIt != redfishUriList.end())
                    {
                        // Found the Redfish URI of the isolated hardware unit.
                        break;
                    }
                }
                if (redfishUriIt != redfishUriList.end())
                {
                    // No need to check in the next service interface list
                    break;
                }
            }

            if (redfishUriIt == redfishUriList.end())
            {
                BMCWEB_LOG_ERROR
                    << "The object[" << dbusObjPath.str
                    << "] interface is not found in the Redfish URI list. "
                    << "Please add the respective D-Bus interface name";
                messages::internalError(asyncResp->res);
                return;
            }

            // Fill the isolated hardware object id along with the Redfish URI
            std::string redfishUri =
                redfishUriIt->second + "/" + getIsolatedHwItemId(dbusObjPath);

            // Make sure whether no need to fill the parent object id in the
            // isolated hardware Redfish URI.
            const std::string uriIdPattern{"<str>"};
            size_t uriIdPos = redfishUri.rfind(uriIdPattern);
            if (uriIdPos == std::string::npos)
            {
                if (entryJsonIdx > 0)
                {
                    asyncResp->res.jsonValue["Members"][entryJsonIdx - 1]
                                            ["Links"]["OriginOfCondition"] = {
                        {"@odata.id", redfishUri}};
                }
                else
                {
                    asyncResp->res.jsonValue["Links"]["OriginOfCondition"] = {
                        {"@odata.id", redfishUri}};
                }
                return;
            }

            bool isChassisAssemblyUri = false;
            std::string::size_type assemblyStartPos =
                redfishUri.rfind("/Assembly#/Assemblies");
            if (assemblyStartPos != std::string::npos)
            {
                // Redfish URI using path segment like DBus object path
                // so using object_path type
                if (sdbusplus::message::object_path(
                        redfishUri.substr(0, assemblyStartPos))
                        .parent_path()
                        .filename() != "Chassis")
                {
                    // Currently, bmcweb supporting only chassis
                    // assembly uri so return error if unsupported
                    // assembly uri added in the redfishUriList.
                    BMCWEB_LOG_ERROR << "Unsupported Assembly URI ["
                                     << redfishUri
                                     << "] to fill in the OriginOfCondition. "
                                     << "Please add support in the bmcweb";
                    messages::internalError(asyncResp->res);
                    return;
                }
                isChassisAssemblyUri = true;
            }

            // Fill the all parents Redfish URI id.
            // For example, the processors id for the core.
            // "/redfish/v1/Systems/system/Processors/<str>/SubProcessors/core0"
            std::vector<std::pair<RedfishResourceDBusInterfaces, size_t>>
                ancestorsIfaces;
            while (uriIdPos != std::string::npos)
            {
                std::string parentRedfishUri =
                    redfishUri.substr(0, uriIdPos - 1);
                RedfishUriListType::const_iterator parentRedfishUriIt =
                    std::find_if(redfishUriList.begin(), redfishUriList.end(),
                                 [parentRedfishUri](const auto& ele) {
                                     return parentRedfishUri == ele.second;
                                 });

                if (parentRedfishUriIt == redfishUriList.end())
                {
                    BMCWEB_LOG_ERROR
                        << "Failed to fill Links:OriginOfCondition "
                        << "because unable to get parent Redfish URI "
                        << "[" << parentRedfishUri << "]"
                        << "DBus interface for the identified "
                        << "Redfish URI: " << redfishUri
                        << " of the given DBus object path: "
                        << dbusObjPath.str;
                    messages::internalError(asyncResp->res);
                    return;
                }
                ancestorsIfaces.emplace_back(
                    std::make_pair(parentRedfishUriIt->first, uriIdPos));
                uriIdPos = redfishUri.rfind(uriIdPattern,
                                            uriIdPos - uriIdPattern.length());
            }

            // GetAncestors only accepts "as" for the interface list
            std::vector<RedfishResourceDBusInterfaces> ancestorsIfacesOnly;
            std::transform(
                ancestorsIfaces.begin(), ancestorsIfaces.end(),
                std::back_inserter(ancestorsIfacesOnly),
                [](const std::pair<RedfishResourceDBusInterfaces, size_t>& p) {
                    return p.first;
                });

            crow::connections::systemBus->async_method_call(
                [asyncResp, dbusObjPath, entryJsonIdx, redfishUri, uriIdPos,
                 uriIdPattern, ancestorsIfaces, isChassisAssemblyUri](
                    const boost::system::error_code ec,
                    const boost::container::flat_map<
                        std::string,
                        boost::container::flat_map<std::string,
                                                   std::vector<std::string>>>&
                        ancestors) mutable {
                    if (ec)
                    {
                        BMCWEB_LOG_ERROR
                            << "DBUS response error [" << ec.value() << " : "
                            << ec.message()
                            << "] when tried to fill the parent "
                            << "objects id in the RedfishURI: " << redfishUri
                            << " of the isolated hareware: " << dbusObjPath.str;
                        messages::internalError(asyncResp->res);
                        return;
                    }

                    // tuple: assembly parent service name, object path, and
                    // interface
                    std::tuple<std::string, sdbusplus::message::object_path,
                               std::string>
                        assemblyParent;
                    for (const auto& ancestorIface : ancestorsIfaces)
                    {
                        bool foundAncestor = false;
                        for (const auto& obj : ancestors)
                        {
                            for (const auto& service : obj.second)
                            {
                                for (const auto& interface : service.second)
                                {
                                    if (interface == ancestorIface.first)
                                    {
                                        foundAncestor = true;
                                        redfishUri.replace(
                                            ancestorIface.second,
                                            uriIdPattern.length(),
                                            getIsolatedHwItemId(
                                                sdbusplus::message::object_path(
                                                    obj.first)));
                                        if (isChassisAssemblyUri &&
                                            interface ==
                                                "xyz.openbmc_project.Inventory."
                                                "Item.Chassis")
                                        {
                                            assemblyParent = std::make_tuple(
                                                service.first,
                                                sdbusplus::message::object_path(
                                                    obj.first),
                                                interface);
                                        }
                                        break;
                                    }
                                }
                                if (foundAncestor)
                                {
                                    break;
                                }
                            }
                            if (foundAncestor)
                            {
                                break;
                            }
                        }

                        if (!foundAncestor)
                        {
                            BMCWEB_LOG_ERROR
                                << "Failed to fill Links:OriginOfCondition "
                                << "because unable to get parent DBus path "
                                << "for the identified parent interface : "
                                << ancestorIface.first
                                << " of the given DBus object path: "
                                << dbusObjPath.str;
                            messages::internalError(asyncResp->res);
                            return;
                        }
                    }

                    if (entryJsonIdx > 0)
                    {
                        asyncResp->res.jsonValue["Members"][entryJsonIdx - 1]
                                                ["Links"]["OriginOfCondition"] =
                            {{"@odata.id", redfishUri}};

                        if (isChassisAssemblyUri)
                        {
                            auto uriPropPath = "/Members"_json_pointer;
                            uriPropPath /= entryJsonIdx - 1;
                            uriPropPath /= "Links";
                            uriPropPath /= "OriginOfCondition";
                            uriPropPath /= "@odata.id";

                            assembly::fillWithAssemblyId(
                                asyncResp, std::get<0>(assemblyParent),
                                std::get<1>(assemblyParent),
                                std::get<2>(assemblyParent), uriPropPath,
                                dbusObjPath, redfishUri);
                        }
                    }
                    else
                    {
                        asyncResp->res.jsonValue["Links"]["OriginOfCondition"] =
                            {{"@odata.id", redfishUri}};

                        if (isChassisAssemblyUri)
                        {
                            auto uriPropPath = "/Links"_json_pointer;
                            uriPropPath /= "OriginOfCondition";
                            uriPropPath /= "@odata.id";

                            assembly::fillWithAssemblyId(
                                asyncResp, std::get<0>(assemblyParent),
                                std::get<1>(assemblyParent),
                                std::get<2>(assemblyParent), uriPropPath,
                                dbusObjPath, redfishUri);
                        }
                    }
                },
                "xyz.openbmc_project.ObjectMapper",
                "/xyz/openbmc_project/object_mapper",
                "xyz.openbmc_project.ObjectMapper", "GetAncestors",
                dbusObjPath.str, ancestorsIfacesOnly);
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetObject", dbusObjPath.str,
        std::array<const char*, 0>{});
}

/**
 * @brief API used to get "PrettyName" by using the given dbus object path
 *        and fill into "Message" property of LogEntry schema.
 *
 * @param[in] asyncResp - The redfish response to return.
 * @param[in] dbusObjPath - The DBus object path which represents redfishUri.
 * @param[in] entryJsonIdx - The json entry index to add isolated hardware
 *                            details in the appropriate entry json object.
 *
 * @return The redfish response with "Message" property of LogEntry schema
 *         if success else nothing in redfish response.
 */

inline void getPrettyNameByDbusObjPath(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const sdbusplus::message::object_path& dbusObjPath,
    const size_t entryJsonIdx)
{
    crow::connections::systemBus->async_method_call(
        [asyncResp, dbusObjPath,
         entryJsonIdx](const boost::system::error_code ec,
                       const GetObjectType& objType) mutable {
            if (ec || objType.empty())
            {
                BMCWEB_LOG_ERROR << "DBUS response error [" << ec.value()
                                 << " : " << ec.message()
                                 << "] when tried to get the dbus name"
                                    "of isolated hareware: "
                                 << dbusObjPath.str;
                messages::internalError(asyncResp->res);
                return;
            }

            if (objType.size() > 1)
            {
                BMCWEB_LOG_ERROR << "More than one dbus service implemented "
                                    "the xyz.openbmc_project.Inventory.Item "
                                    "interface to get the PrettyName";
                messages::internalError(asyncResp->res);
                return;
            }

            if (objType[0].first.empty())
            {
                BMCWEB_LOG_ERROR << "The retrieved dbus name is empty for the "
                                    "given dbus object: "
                                 << dbusObjPath.str;
                messages::internalError(asyncResp->res);
                return;
            }

            if (entryJsonIdx > 0)
            {
                asyncResp->res
                    .jsonValue["Members"][entryJsonIdx - 1]["Message"] =
                    dbusObjPath.filename();
                auto msgPropPath = "/Members"_json_pointer;
                msgPropPath /= entryJsonIdx - 1;
                msgPropPath /= "Message";
                name_util::getPrettyName(asyncResp, dbusObjPath.str, objType,
                                         msgPropPath);
            }
            else
            {
                asyncResp->res.jsonValue["Message"] = dbusObjPath.filename();
                name_util::getPrettyName(asyncResp, dbusObjPath.str, objType,
                                         "/Message"_json_pointer);
            }
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetObject", dbusObjPath.str,
        std::array<const char*, 1>{"xyz.openbmc_project.Inventory.Item"});
}

/**
 * @brief API used to fill the isolated hardware details into LogEntry schema
 *        by using the given isolated dbus object which is present in
 *        xyz.openbmc_project.Association.Definitions::Associations of the
 *        HardwareIsolation dbus entry object.
 *
 * @param[in] asyncResp - The redfish response to return.
 * @param[in] dbusObjPath - The DBus object path which represents redfishUri.
 * @param[in] entryJsonIdx - The json entry index to add isolated hardware
 *                            details in the appropriate entry json object.
 *
 * @return The redfish response with appropriate redfish properties of the
 *         isolated hardware details into LogEntry schema if success else
 *         nothing in redfish response.
 */
inline void fillIsolatedHwDetailsByObjPath(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const sdbusplus::message::object_path& dbusObjPath,
    const size_t entryJsonIdx)
{
    // Fill Redfish uri of isolated hardware into "OriginOfCondition"
    if (dbusObjPath.filename().find("unit") != std::string::npos)
    {
        // If Isolated Hardware object name contain "unit" then that unit
        // is not modelled in inventory and redfish so the "OriginOfCondition"
        // should filled with it's parent (aka FRU of unit) path.
        getRedfishUriByDbusObjPath(asyncResp, dbusObjPath.parent_path(),
                                   entryJsonIdx);
    }
    else
    {
        getRedfishUriByDbusObjPath(asyncResp, dbusObjPath, entryJsonIdx);
    }

    // Fill PrettyName of isolated hardware into "Message"
    getPrettyNameByDbusObjPath(asyncResp, dbusObjPath, entryJsonIdx);
}

/**
 * @brief API used to fill isolated hardware details into LogEntry schema
 *        by using the given isolated dbus object.
 *
 * @param[in] asyncResp - The redfish response to return.
 * @param[in] entryJsonIdx - The json entry index to add isolated hardware
 *                            details. If passing less than or equal 0 then,
 *                            it will assume the given asyncResp jsonValue as
 *                            a single entry json object. If passing greater
 *                            than 0 then, it will assume the given asyncResp
 *                            jsonValue contains "Members" to fill in the
 *                            appropriate entry json object.
 * @param[in] dbusObjIt - The DBus object which contains isolated hardware
                         details.
 *
 * @return The redfish response with appropriate redfish properties of the
 *         isolated hardware details into LogEntry schema if success else
 *         failure response.
 */
inline void fillSystemHardwareIsolationLogEntry(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const size_t entryJsonIdx, GetManagedObjectsType::const_iterator& dbusObjIt)
{
    nlohmann::json& entryJson =
        (entryJsonIdx > 0
             ? asyncResp->res.jsonValue["Members"][entryJsonIdx - 1]
             : asyncResp->res.jsonValue);

    for (auto& interface : dbusObjIt->second)
    {
        if (interface.first == "xyz.openbmc_project."
                               "HardwareIsolation.Entry")
        {
            for (auto& property : interface.second)
            {
                if (property.first == "Severity")
                {
                    const std::string* severity =
                        std::get_if<std::string>(&property.second);
                    if (severity == nullptr)
                    {
                        BMCWEB_LOG_ERROR
                            << "Failed to get the Severity "
                            << "from object: " << dbusObjIt->first.str;
                        messages::internalError(asyncResp->res);
                        break;
                    }

                    if (*severity == "xyz.openbmc_project."
                                     "HardwareIsolation.Entry.Type."
                                     "Critical")
                    {
                        entryJson["Severity"] = "Critical";
                    }
                    else if (*severity == "xyz.openbmc_project."
                                          "HardwareIsolation."
                                          "Entry.Type.Warning")
                    {
                        entryJson["Severity"] = "Warning";
                    }
                    else if (*severity == "xyz.openbmc_project."
                                          "HardwareIsolation."
                                          "Entry.Type.Manual")
                    {
                        entryJson["Severity"] = "OK";
                    }
                    else
                    {
                        BMCWEB_LOG_ERROR
                            << "Unsupported Severity[ " << *severity
                            << "] from object: " << dbusObjIt->first.str;
                        messages::internalError(asyncResp->res);
                        break;
                    }
                }
            }
        }
        else if (interface.first == "xyz.openbmc_project."
                                    "Time.EpochTime")
        {
            for (auto& property : interface.second)
            {
                if (property.first == "Elapsed")
                {
                    const uint64_t* elapsdTime =
                        std::get_if<uint64_t>(&property.second);
                    if (elapsdTime == nullptr)
                    {
                        BMCWEB_LOG_ERROR
                            << "Failed to get the Elapsed time "
                            << "from object: " << dbusObjIt->first.str;
                        messages::internalError(asyncResp->res);
                        break;
                    }
                    entryJson["Created"] = crow::utility::getDateTime(
                        static_cast<std::time_t>(*elapsdTime));
                }
            }
        }
        else if (interface.first == "xyz.openbmc_project.Association."
                                    "Definitions")
        {
            for (auto& property : interface.second)
            {
                if (property.first == "Associations")
                {
                    const AssociationsValType* associations =
                        std::get_if<AssociationsValType>(&property.second);
                    if (associations == nullptr)
                    {
                        BMCWEB_LOG_ERROR
                            << "Failed to get the Associations"
                            << "from object: " << dbusObjIt->first.str;
                        messages::internalError(asyncResp->res);
                        break;
                    }
                    for (auto& assoc : *associations)
                    {
                        if (std::get<0>(assoc) == "isolated_hw")
                        {
                            fillIsolatedHwDetailsByObjPath(
                                asyncResp,
                                sdbusplus::message::object_path(
                                    std::get<2>(assoc)),
                                entryJsonIdx);
                        }
                        else if (std::get<0>(assoc) == "isolated_hw_errorlog")
                        {
                            sdbusplus::message::object_path errPath =
                                std::get<2>(assoc);

                            // Set error log uri based on the error log hidden
                            // property
                            if (entryJsonIdx > 0)
                            {
                                nlohmann::json_pointer errorLogPropPath =
                                    "/Members"_json_pointer;
                                errorLogPropPath /= entryJsonIdx - 1;
                                errorLogPropPath /= "AdditionalDataURI";
                                error_log_utils::setErrorLogUri(
                                    asyncResp, errPath, errorLogPropPath,
                                    false);
                            }
                            else
                            {
                                error_log_utils::setErrorLogUri(
                                    asyncResp, errPath,
                                    "/AdditionalDataURI"_json_pointer, false);
                            }
                        }
                    }
                }
            }
        }
    }

    entryJson["@odata.type"] = "#LogEntry.v1_9_0.LogEntry";
    entryJson["@odata.id"] =
        "/redfish/v1/Systems/system/LogServices/HardwareIsolation/"
        "Entries/" +
        dbusObjIt->first.filename();
    entryJson["Id"] = dbusObjIt->first.filename();
    entryJson["Name"] = "Hardware Isolation Entry";
    entryJson["EntryType"] = "Event";
}

/**
 * @brief API Used to add the supported HardwareIsolation LogEntry Entries id
 *
 * @param[in] req - The HardwareIsolation redfish request (unused now).
 * @param[in] asyncResp - The redfish response to return.
 *
 * @return The redfish response in the given buffer.
 *
 * @note This function will return the available entries dbus object which are
 *       created by HardwareIsolation manager.
 */
inline void getSystemHardwareIsolationLogEntryCollection(
    const crow::Request& /* req */,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{

    auto getManagedObjectsHandler = [asyncResp](
                                        const boost::system::error_code ec,
                                        const GetManagedObjectsType& mgtObjs) {
        if (ec)
        {
            BMCWEB_LOG_ERROR
                << "DBUS response error [" << ec.value() << " : "
                << ec.message()
                << "] when tried to get the HardwareIsolation managed objects";
            messages::internalError(asyncResp->res);
            return;
        }

        nlohmann::json& entriesArray = asyncResp->res.jsonValue["Members"];
        entriesArray = nlohmann::json::array();

        for (auto dbusObjIt = mgtObjs.begin(); dbusObjIt != mgtObjs.end();
             dbusObjIt++)
        {
            if (dbusObjIt->second.find(
                    "xyz.openbmc_project.HardwareIsolation.Entry") ==
                dbusObjIt->second.end())
            {
                // The retrieved object is not hardware isolation entry
                continue;
            }
            entriesArray.push_back(nlohmann::json::object());

            fillSystemHardwareIsolationLogEntry(asyncResp, entriesArray.size(),
                                                dbusObjIt);
        }

        asyncResp->res.jsonValue["Members@odata.count"] = entriesArray.size();

        asyncResp->res.jsonValue["@odata.type"] =
            "#LogEntryCollection.LogEntryCollection";
        asyncResp->res.jsonValue["@odata.id"] =
            "/redfish/v1/Systems/system/LogServices/HardwareIsolation/"
            "Entries";
        asyncResp->res.jsonValue["Name"] = "Hardware Isolation Entries";
        asyncResp->res.jsonValue["Description"] =
            "Collection of System Hardware Isolation Entries";
    };

    // Get the DBus name of HardwareIsolation service
    crow::connections::systemBus->async_method_call(
        [asyncResp, getManagedObjectsHandler](
            const boost::system::error_code ec, const GetObjectType& objType) {
            if (ec || objType.empty())
            {
                BMCWEB_LOG_ERROR << "DBUS response error [" << ec.value()
                                 << " : " << ec.message()
                                 << "] when tried to get the HardwareIsolation "
                                    "dbus name";
                messages::internalError(asyncResp->res);
                return;
            }

            if (objType.size() > 1)
            {
                BMCWEB_LOG_ERROR << "More than one dbus service implemented "
                                    "the HardwareIsolation service";
                messages::internalError(asyncResp->res);
                return;
            }

            if (objType[0].first.empty())
            {
                BMCWEB_LOG_ERROR << "The retrieved HardwareIsolation dbus "
                                    "name is empty";
                messages::internalError(asyncResp->res);
                return;
            }

            // Fill the Redfish LogEntry schema for the retrieved
            // HardwareIsolation entries
            crow::connections::systemBus->async_method_call(
                getManagedObjectsHandler, objType[0].first,
                "/xyz/openbmc_project/hardware_isolation",
                "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetObject",
        "/xyz/openbmc_project/hardware_isolation",
        std::array<const char*, 1>{
            "xyz.openbmc_project.HardwareIsolation.Create"});
}

/**
 * @brief API Used to fill LogEntry schema by using the HardwareIsolation dbus
 *        entry object which will get by using the given entry id in redfish
 *        uri.
 *
 * @param[in] req - The HardwareIsolation redfish request (unused now).
 * @param[in] asyncResp - The redfish response to return.
 * @param[in] entryId - The entry id of HardwareIsolation entries to retrieve
 *                      the corresponding isolated hardware details.
 *
 * @return The redfish response in the given buffer with LogEntry schema
 *         members if success else will error.
 */
inline void getSystemHardwareIsolationLogEntryById(
    const crow::Request& /* req */,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& entryId)
{
    sdbusplus::message::object_path entryObjPath(
        std::string("/xyz/openbmc_project/hardware_isolation/entry") + "/" +
        entryId);

    auto getManagedObjectsRespHandler = [asyncResp, entryObjPath](
                                            const boost::system::error_code ec,
                                            const GetManagedObjectsType&
                                                mgtObjs) {
        if (ec)
        {
            BMCWEB_LOG_ERROR
                << "DBUS response error [" << ec.value() << " : "
                << ec.message()
                << "] when tried to get the HardwareIsolation managed objects";
            messages::internalError(asyncResp->res);
            return;
        }

        bool entryIsPresent = false;
        for (auto dbusObjIt = mgtObjs.begin(); dbusObjIt != mgtObjs.end();
             dbusObjIt++)
        {
            if (dbusObjIt->first == entryObjPath)
            {
                entryIsPresent = true;
                fillSystemHardwareIsolationLogEntry(asyncResp, 0, dbusObjIt);
                break;
            }
        }

        if (!entryIsPresent)
        {
            messages::resourceNotFound(asyncResp->res, "Entry",
                                       entryObjPath.filename());
            return;
        }
    };

    auto getObjectRespHandler = [asyncResp, entryId, entryObjPath,
                                 getManagedObjectsRespHandler](
                                    const boost::system::error_code ec,
                                    const GetObjectType& objType) {
        if (ec || objType.empty())
        {
            BMCWEB_LOG_ERROR << "DBUS response error [" << ec.value() << " : "
                             << ec.message()
                             << "] when tried to get the HardwareIsolation "
                                "dbus name the given object path: "
                             << entryObjPath.str;
            if (ec.value() == EBADR)
            {
                messages::resourceNotFound(asyncResp->res, "Entry", entryId);
            }
            else
            {
                messages::internalError(asyncResp->res);
            }
            return;
        }

        if (objType.size() > 1)
        {
            BMCWEB_LOG_ERROR << "More than one dbus service implemented "
                                "the HardwareIsolation service";
            messages::internalError(asyncResp->res);
            return;
        }

        if (objType[0].first.empty())
        {
            BMCWEB_LOG_ERROR << "The retrieved HardwareIsolation dbus "
                                "name is empty";
            messages::internalError(asyncResp->res);
            return;
        }

        // Fill the Redfish LogEntry schema for the identified entry dbus object
        crow::connections::systemBus->async_method_call(
            getManagedObjectsRespHandler, objType[0].first,
            "/xyz/openbmc_project/hardware_isolation",
            "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    };

    // Make sure the given entry id is present in hardware isolation
    // dbus entries and get the DBus name of that entry to fill LogEntry
    crow::connections::systemBus->async_method_call(
        getObjectRespHandler, "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetObject", entryObjPath.str,
        hwIsolationEntryIfaces);
}

/**
 * @brief API Used to deisolate the given HardwareIsolation entry.
 *
 * @param[in] req - The HardwareIsolation redfish request (unused now).
 * @param[in] asyncResp - The redfish response to return.
 * @param[in] entryId - The entry id of HardwareIsolation entries to deisolate.
 *
 * @return The redfish response in the given buffer.
 */
inline void deleteSystemHardwareIsolationLogEntryById(
    const crow::Request& /* req */,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& entryId)
{
    sdbusplus::message::object_path entryObjPath(
        std::string("/xyz/openbmc_project/hardware_isolation/entry") + "/" +
        entryId);

    // Make sure the given entry id is present in hardware isolation
    // entries and get the DBus name of that entry
    crow::connections::systemBus->async_method_call(
        [asyncResp, entryId, entryObjPath](const boost::system::error_code ec,
                                           const GetObjectType& objType) {
            if (ec || objType.empty())
            {
                BMCWEB_LOG_ERROR << "DBUS response error [" << ec.value()
                                 << " : " << ec.message()
                                 << "] when tried to get the HardwareIsolation "
                                    "dbus name the given object path: "
                                 << entryObjPath.str;
                if (ec.value() == EBADR)
                {
                    messages::resourceNotFound(asyncResp->res, "Entry",
                                               entryId);
                }
                else
                {
                    messages::internalError(asyncResp->res);
                }
                return;
            }

            if (objType.size() > 1)
            {
                BMCWEB_LOG_ERROR << "More than one dbus service implemented "
                                    "the HardwareIsolation service";
                messages::internalError(asyncResp->res);
                return;
            }

            if (objType[0].first.empty())
            {
                BMCWEB_LOG_ERROR << "The retrieved HardwareIsolation dbus "
                                    "name is empty";
                messages::internalError(asyncResp->res);
                return;
            }

            // Delete the respective dbus entry object
            crow::connections::systemBus->async_method_call(
                [asyncResp,
                 entryObjPath](const boost::system::error_code ec,
                               const sdbusplus::message::message& msg) {
                    if (!ec)
                    {
                        messages::success(asyncResp->res);
                        return;
                    }

                    BMCWEB_LOG_ERROR << "DBUS response error [" << ec.value()
                                     << " : " << ec.message()
                                     << "] when tried to delete the given "
                                     << "entry: " << entryObjPath.str;

                    const sd_bus_error* dbusError = msg.get_error();

                    if (dbusError == nullptr)
                    {
                        messages::internalError(asyncResp->res);
                        return;
                    }

                    BMCWEB_LOG_ERROR << "DBus ErrorName: " << dbusError->name
                                     << " ErrorMsg: " << dbusError->message;

                    if (strcmp(dbusError->name,
                               "xyz.openbmc_project.Common.Error."
                               "NotAllowed") == 0)
                    {
                        messages::chassisPowerStateOffRequired(asyncResp->res,
                                                               "chassis");
                    }
                    else if (strcmp(dbusError->name,
                                    "xyz.openbmc_project.Common.Error."
                                    "InsufficientPermission") == 0)
                    {
                        messages::resourceCannotBeDeleted(asyncResp->res);
                    }
                    else
                    {
                        BMCWEB_LOG_ERROR << "DBus Error is unsupported so "
                                            "returning as Internal Error";
                        messages::internalError(asyncResp->res);
                    }
                    return;
                },
                objType[0].first, entryObjPath.str,
                "xyz.openbmc_project.Object.Delete", "Delete");
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetObject", entryObjPath.str,
        hwIsolationEntryIfaces);
}

/**
 * @brief API Used to deisolate the all HardwareIsolation entries.
 *
 * @param[in] req - The HardwareIsolation redfish request (unused now).
 * @param[in] asyncResp - The redfish response to return.
 *
 * @return The redfish response in the given buffer.
 */
inline void postSystemHardwareIsolationLogServiceClearLog(
    const crow::Request& /* req */,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    // Get the DBus name of HardwareIsolation service
    crow::connections::systemBus->async_method_call(
        [asyncResp](const boost::system::error_code ec,
                    const GetObjectType& objType) {
            if (ec || objType.empty())
            {
                BMCWEB_LOG_ERROR << "DBUS response error [" << ec.value()
                                 << " : " << ec.message()
                                 << "] when tried to get the HardwareIsolation "
                                    "dbus name";
                messages::internalError(asyncResp->res);
                return;
            }

            if (objType.size() > 1)
            {
                BMCWEB_LOG_ERROR << "More than one dbus service implemented "
                                    "the HardwareIsolation service";
                messages::internalError(asyncResp->res);
                return;
            }

            if (objType[0].first.empty())
            {
                BMCWEB_LOG_ERROR << "The retrieved HardwareIsolation dbus "
                                    "name is empty";
                messages::internalError(asyncResp->res);
                return;
            }

            // Delete all HardwareIsolation entries
            crow::connections::systemBus->async_method_call(
                [asyncResp](const boost::system::error_code ec,
                            const sdbusplus::message::message& msg) {
                    if (!ec)
                    {
                        messages::success(asyncResp->res);
                        return;
                    }

                    BMCWEB_LOG_ERROR
                        << "DBUS response error [" << ec.value() << " : "
                        << ec.message()
                        << "] when tried to delete all HardwareIsolation "
                           "entries";

                    const sd_bus_error* dbusError = msg.get_error();

                    if (dbusError == nullptr)
                    {
                        messages::internalError(asyncResp->res);
                        return;
                    }

                    BMCWEB_LOG_ERROR << "DBus ErrorName: " << dbusError->name
                                     << " ErrorMsg: " << dbusError->message;

                    if (strcmp(dbusError->name,
                               "xyz.openbmc_project.Common.Error."
                               "NotAllowed") == 0)
                    {
                        messages::chassisPowerStateOffRequired(asyncResp->res,
                                                               "chassis");
                    }
                    else
                    {
                        BMCWEB_LOG_ERROR << "DBus Error is unsupported so "
                                            "returning as Internal Error";
                        messages::internalError(asyncResp->res);
                    }
                    return;
                },
                objType[0].first, "/xyz/openbmc_project/hardware_isolation",
                "xyz.openbmc_project.Collection.DeleteAll", "DeleteAll");
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetObject",
        "/xyz/openbmc_project/hardware_isolation",
        std::array<const char*, 1>{"xyz.openbmc_project.Collection.DeleteAll"});
}

/**
 * @brief API used to route the handler for HardwareIsolation Redfish
 *        LogServices URI
 *
 * @param[in] app - Crow app on which Redfish will initialize
 *
 * @return The appropriate redfish response for the given redfish request.
 */
inline void requestRoutesSystemHardwareIsolationLogService(App& app)
{
    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/system/LogServices/HardwareIsolation/")
        .privileges(redfish::privileges::getLogService)
        .methods(boost::beast::http::verb::get)(
            getSystemHardwareIsolationLogService);

    BMCWEB_ROUTE(
        app, "/redfish/v1/Systems/system/LogServices/HardwareIsolation/Entries")
        .privileges(redfish::privileges::getLogEntryCollection)
        .methods(boost::beast::http::verb::get)(
            getSystemHardwareIsolationLogEntryCollection);

    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/"
                      "HardwareIsolation/Entries/<str>/")
        .privileges(redfish::privileges::getLogEntry)
        .methods(boost::beast::http::verb::get)(
            getSystemHardwareIsolationLogEntryById);

    BMCWEB_ROUTE(app, "/redfish/v1/Systems/system/LogServices/"
                      "HardwareIsolation/Entries/<str>/")
        .privileges(redfish::privileges::deleteLogEntry)
        .methods(boost::beast::http::verb::delete_)(
            deleteSystemHardwareIsolationLogEntryById);

    BMCWEB_ROUTE(app,
                 "/redfish/v1/Systems/system/LogServices/HardwareIsolation/"
                 "Actions/LogService.ClearLog/")
        .privileges(redfish::privileges::postLogService)
        .methods(boost::beast::http::verb::post)(
            postSystemHardwareIsolationLogServiceClearLog);
}
#endif // BMCWEB_ENABLE_HW_ISOLATION

} // namespace redfish
