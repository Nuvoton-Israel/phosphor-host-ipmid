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

#include "smbiosmdrv2handler.hpp"

#include <ipmid/api.hpp>
#include <ipmid/utils.hpp>
#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/log.hpp>
#include <string>
#include <xyz/openbmc_project/Common/error.hpp>
#include <fstream>

std::unique_ptr<MDRV2> mdrv2 = nullptr;

void register_netfn_smbiosmdrv2_functions() __attribute__((constructor));

void SharedMemoryArea::Initialize(uint32_t addr, uint32_t areaSize)
{
    int memDriver = 0;

    // open mem driver for the system memory access
    memDriver = open("/dev/mem", O_RDONLY);
    if (memDriver < 0)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Cannot access mem driver");
        throw std::system_error(EIO, std::generic_category());
    }

    // map the system memory
    vPtr = mmap(NULL,                       // where to map to: don't mind
                areaSize,                   // how many bytes ?
                PROT_READ,                  // want to read and write
                MAP_SHARED,                 // no copy on write
                memDriver,                  // handle to /dev/mem
                physicalAddr);              // hopefully the Text-buffer :-)

    close(memDriver);
    if (vPtr == MAP_FAILED)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Failed to map share memory");
        throw std::system_error(EIO, std::generic_category());
    }
    size = areaSize;
    physicalAddr = addr;
}

void MDRV2::timeoutHandler()
{
    mdrv2->area.reset(nullptr);
}

void MDRV2::RestartMDRV2()
{
    std::shared_ptr<sdbusplus::asio::connection> bus = getSdBus();
    sdbusplus::message::message method =
        bus->new_method_call(SYSTEMD_SERVICE, SYSTEMD_OBJ_PATH, SYSTEMD_INTERFACE,
                             "RestartUnit");

    method.append(SMBIOS_MDRV2_SERVICE);
    method.append("replace");

    bus->call_noreply(method);
}

bool MDRV2::storeDatatoFlash(MDRSMBIOSHeader *mdrHdr, uint8_t *data)
{
    std::ofstream smbiosFile(mdrType2File,
                             std::ios_base::binary | std::ios_base::trunc);
    if (!smbiosFile.good())
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Write data from flash error - Open MDRV2 table file failure");
        return false;
    }

    try
    {
        smbiosFile.write(reinterpret_cast<char *>(mdrHdr),
                         sizeof(MDRSMBIOSHeader));
        smbiosFile.write(reinterpret_cast<char *>(data), mdrHdr->dataSize);
    }
    catch (std::ofstream::failure &e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Write data from flash error - write data error",
            phosphor::logging::entry("ERROR=%s", e.what()));
        return false;
    }

    return true;
}

/** @brief implements mdr2 get mbox shared mem command
 *
 *  @returns IPMI completion code
 */
ipmi::RspType<> cmd_mdr2_get_mbox_shared_mem()
{
    std::shared_ptr<sdbusplus::asio::connection> bus = getSdBus();
    std::string service = ipmi::getService(*bus, mdrv2Interface, mdrv2Path);

    if (mdrv2 == nullptr)
    {
        mdrv2 = std::make_unique<MDRV2>();
    }

    try
    {
        mdrv2->area =
            std::make_unique<SharedMemoryArea>(MboxAddress, MboxLength);
    }
    catch (const std::system_error &e)
    {
        return ipmi::responseUnspecifiedError();
    }

    std::vector<std::uint8_t> results(MboxLength);
    std::memcpy(results.data(), mdrv2->area->vPtr, MboxLength);

    mdrv2->area.reset(nullptr);
    MDRSMBIOSHeader mdr2Smbios;
    mdr2Smbios.mdrType = mdrTypeII;
    mdr2Smbios.dirVer = mdrv2->smbiosDir.dir[0].common.dataVersion;
    mdr2Smbios.timestamp = mdrv2->smbiosDir.dir[0].common.timestamp;
    mdr2Smbios.dataSize = mdrv2->smbiosDir.dir[0].common.size;

    if (access(smbiosPath, 0) == -1)
    {
        int flag = mkdir(smbiosPath, S_IRWXU);
        if (flag != 0)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "create folder failed for writting smbios file");
        }
    }
    if (!mdrv2->storeDatatoFlash(&mdr2Smbios, results.data()))
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "MDR2 Store data to flash failed");
        return ipmi::responseDestinationUnavailable();
    }

    mdrv2->RestartMDRV2();
    return ipmi::responseSuccess();
}

void register_netfn_smbiosmdrv2_functions()
{
    // <Get Mailbox Shared Memory>
    ipmi::registerHandler(ipmi::prioOpenBmcBase, ipmi::intel::netFnApp,
                          ipmi::intel::app::cmdMdrIIGetMboxSharedMem, ipmi::Privilege::Operator,
                          cmd_mdr2_get_mbox_shared_mem);
    return;
}
