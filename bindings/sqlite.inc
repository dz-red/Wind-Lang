static const WindCFunction sqlite_functions[] = {
    {.name = "version", .symbol = "sqlite3_libversion", .result = TK_KW_STR, .copy_string = 1},
    {0}
};
