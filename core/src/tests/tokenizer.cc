/*
  BAREOS® - Backup Archiving REcovery Open Sourced

  Copyright (C) 2024-2024 Bareos GmbH & Co. KG

  This program is Free Software; you can redistribute it and/or
  modify it under the terms of version three of the GNU Affero General Public
  License as published by the Free Software Foundation, which is
  listed in the file LICENSE.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
  02110-1301, USA.
*/

#include "lib/lex.h"
#include <cstdio>

LEX* lex_add(LEX* lf,
             const char* filename,
             FILE* fd,
             Bpipe* bpipe,
             LEX_ERROR_HANDLER* ScanError,
             LEX_WARNING_HANDLER* scan_warning);

int main()
{
  auto* lex = lex_add(NULL, "stdin", stdin, NULL, NULL, NULL);

  if (!lex) {
    printf("Something went really wrong!\n");
    return 1;
  }

  for (size_t num = 0;; ++num) {
    auto token = LexGetToken(lex, BCT_ALL);

    if (token == BCT_ERROR) {
      printf("Error received\n");
      break;
    }

    if (token == BCT_EOF) { break; }

    printf("%zu: line: %d:%d token:%s (%s)\n", num, lex->line_no, lex->col_no,
           lex_tok_to_str(token), lex->str);
  }

  LexCloseFile(lex);
}
