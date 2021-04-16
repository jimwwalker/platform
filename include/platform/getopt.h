/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2018 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */
#pragma once

/**
 * This file contains a getopt implementation which is only used
 * on Windows (but it is built on all platforms to make it easier
 * to test ;)
 */


namespace cb::getopt {

const int no_argument = 0;
const int required_argument = 1;
const int optional_argument = 2;

struct option {
    const char* name;
    int has_arg;
    int* flag;
    int val;
};

extern char* optarg;
extern int opterr;
extern int optind;
extern int optopt;

extern int getopt_long(int argc,
                       char** argv,
                       const char* optstring,
                       const struct option* longopts,
                       int* longindex);

extern int getopt(int argc, char** argv, const char* optstring);

/**
 * This is for unit tests only and used to reset the internal state
 * of the library
 */
void reset();

/**
 * This is used for unit tests to mute the unit tests from writing error
 * messages to stderr
 */
void mute_stderr();
} // namespace cb::getopt
