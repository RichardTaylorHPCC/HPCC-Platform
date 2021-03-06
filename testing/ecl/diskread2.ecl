/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

//UseStandardFiles
// straight disk count

string c1 := 'CLAIRE' : stored('c1');
string c2 := 'CLAIRE' : stored('c2');
string c3 := 'CLAIRE' : stored('c3');
string c4 := 'CLAIRE' : stored('c4');

recordof(DG_FlatFile) createError := TRANSFORM
    SELF.DG_firstname := 'LIMIT EXCEEDED';
    SELF := [];
END;

o1 := output(LIMIT(DG_FlatFile(DG_firstname=c1), 2, skip)) : independent;
o2 := output(LIMIT(DG_FlatFile(DG_firstname=c2), 2, ONFAIL(createError))) : independent;
o3 := count(LIMIT(DG_FlatFile(DG_firstname=c3), 2, skip)) : independent;
o4 := count(LIMIT(DG_FlatFile(DG_firstname=c4), 2, ONFAIL(createError))) : independent;

o1; o2; o3; o4;

