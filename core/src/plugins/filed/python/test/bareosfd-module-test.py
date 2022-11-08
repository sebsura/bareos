#   BAREOS - Backup Archiving REcovery Open Sourced
#
#   Copyright (C) 2020-2022 Bareos GmbH & Co. KG
#
#   This program is Free Software; you can redistribute it and/or
#   modify it under the terms of version three of the GNU Affero General Public
#   License as published by the Free Software Foundation and included
#   in the file LICENSE.
#
#   This program is distributed in the hope that it will be useful, but
#   WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
#   Affero General Public License for more details.
#
#   You should have received a copy of the GNU Affero General Public License
#   along with this program; if not, write to the Free Software
#   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
#   02110-1301, USA.

import bareosfd
import glob

def load_bareos_plugin(plugindef):
    print("Hello from load_bareos_plugin")
    print(plugindef)
    print(bareosfd)

    bareosfd.DebugMessage(100, "Kuckuck")
    bareosfd.JobMessage(100, "Kuckuck")
    #filename_bytes = glob.glob(b'filename-with*')[0]
    filename_bytes = b'filename-with-non-utf8-bytestring->C\303N'
    bareosfd.DebugMessage(100, repr(type(filename_bytes)))
    bareosfd.DebugMessage(100, filename_bytes )

    #filename_string = glob.glob('filename-with*')[0]
    filename_string = 'filename-with-non-utf8-bytestring->C\303N'
    bareosfd.DebugMessage(100, repr(type(filename_string)))
    bareosfd.DebugMessage(100, filename_string)
