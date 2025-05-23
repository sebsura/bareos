#   BAREOS® - Backup Archiving REcovery Open Sourced
#
#   Copyright (C) 2024-2025 Bareos GmbH & Co. KG
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

find_program(SQLCMD NAMES sqlcmd.exe)
get_filename_component(BASENAME ${CMAKE_CURRENT_BINARY_DIR} NAME)
create_systemtest(${SYSTEMTEST_PREFIX} ${BASENAME})

systemtest_requires_setup(01-DBNormal-prepare)
systemtest_requires(02-DBNormal-FullBackup 01-DBNormal-prepare)
systemtest_requires(03-DBNormal-IncBackup 02-DBNormal-FullBackup)
systemtest_requires(04-DBNormal-DropDB 03-DBNormal-IncBackup)
systemtest_requires(05-DBNormal-Restore 04-DBNormal-DropDB)

systemtest_requires_setup(11-DBFileStream-prepare)
systemtest_requires(12-DBFileStream-FullBackup 11-DBFileStream-prepare)
systemtest_requires(13-DBFileStream-IncBackup 12-DBFileStream-FullBackup)
systemtest_requires(14-DBFileStream-DropDB 13-DBFileStream-IncBackup)
systemtest_requires(15-DBFileStream-Restore 14-DBFileStream-DropDB)
