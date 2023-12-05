#!/usr/bin/env python
# -*- coding: utf-8 -*-
# BAREOS - Backup Archiving REcovery Open Sourced
#
# Copyright (C) 2023-2023 Bareos GmbH & Co. KG
#
# This program is Free Software; you can redistribute it and/or
# modify it under the terms of version three of the GNU Affero General Public
# License as published by the Free Software Foundation, which is
# listed in the file LICENSE.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
# 02110-1301, USA.
#

# this test module will emit a job message in every single callback there is
# to make sure emitting a message won't break anything

# import all the wrapper functions in our module scope
from BareosFdWrapper import *

from bareosfd import (
    bRC_OK,
    bRC_More,
    JobMessage,
    DebugMessage,
    M_INFO,
    M_WARNING,
    bFileType,
    StatPacket,
    FT_REG,
)

from BareosFdPluginBaseclass import BareosFdPluginBaseclass

import sys
from stat import S_IFREG, S_IFDIR, S_IRWXU


@BareosPlugin
class TestConfigPlugin(BareosFdPluginBaseclass):
    mandatory_options = []

    def __init__(self, plugindef):
        super().__init__(plugindef)

    def parse_plugin_definition(self, plugindef):
        JobMessage(M_INFO, "plugindef: {}\n".format(plugindef))
        res = super().parse_plugin_definition(plugindef)
        for k, v in self.options.items():
            JobMessage(M_INFO, "effective configuration: {} = '{}'\n".format(k, v))
        return res

    def start_backup_file(self, savepkt):
        statp = StatPacket()
        statp.st_size = 65 * 1024
        statp.st_mode = S_IRWXU | S_IFREG
        savepkt.statp = statp
        savepkt.type = FT_REG
        savepkt.no_read = False
        savepkt.fname = "/file"
        return bRC_OK

    def plugin_io_read(self, IOP):
        IOP.buf = bytearray(IOP.count)
        IOP.io_errno = 0
        IOP.status = 0
        return bRC_OK

    def plugin_io_open(self, IOP):
        return bRC_OK

    def plugin_io_close(self, IOP):
        return bRC_OK
