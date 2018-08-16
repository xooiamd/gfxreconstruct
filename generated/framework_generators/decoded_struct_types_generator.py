#!/usr/bin/python3 -i
#
# Copyright (c) 2018 LunarG, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os,re,sys
from base_generator import *

class DecodedStructTypesGeneratorOptions(BaseGeneratorOptions):
    """Options for decoded Vulkan API structure wrapper C++ code generation"""
    def __init__(self,
                 blacklists = None,         # Path to JSON file listing apicalls and structs to ignore.
                 platformTypes = None,      # Path to JSON file listing platform (WIN32, X11, etc.) defined types.
                 filename = None,
                 directory = '.',
                 prefixText = '',
                 protectFile = False,
                 protectFeature = True):
        BaseGeneratorOptions.__init__(self, blacklists, platformTypes,
                                      filename, directory, prefixText,
                                      protectFile, protectFeature)

# DecodedStructTypesGenerator - subclass of BaseGenerator.
# Generates C++ type declarations for the decoded Vulkan API structure wrappers.
class DecodedStructTypesGenerator(BaseGenerator):
    """Generate decoded structure wrapper C++ type declarations"""
    def __init__(self,
                 errFile = sys.stderr,
                 warnFile = sys.stderr,
                 diagFile = sys.stdout):
        BaseGenerator.__init__(self, errFile, warnFile, diagFile)

    # Method override
    def beginFile(self, genOpts):
        BaseGenerator.beginFile(self, genOpts)

        write('#include <memory>', file=self.outFile)
        self.newline()
        write('#include "vulkan/vulkan.h"', file=self.outFile)
        self.newline()
        write('#include "util/defines.h"', file=self.outFile)
        write('#include "format/pnext_node.h"', file=self.outFile)
        write('#include "format/pointer_decoder.h"', file=self.outFile)
        write('#include "format/string_array_decoder.h"', file=self.outFile)
        write('#include "format/string_decoder.h"', file=self.outFile)
        write('#include "format/struct_pointer_decoder.h"', file=self.outFile)
        self.newline()
        write('BRIMSTONE_BEGIN_NAMESPACE(brimstone)', file=self.outFile)
        write('BRIMSTONE_BEGIN_NAMESPACE(format)', file=self.outFile)

    # Method override
    def endFile(self):
        self.newline()
        write('BRIMSTONE_END_NAMESPACE(format)', file=self.outFile)
        write('BRIMSTONE_END_NAMESPACE(brimstone)', file=self.outFile)

        # Finish processing in superclass
        BaseGenerator.endFile(self)

    #
    # Indicates that the current feature has C++ code to generate.
    def needFeatureGeneration(self):
        if self.featureStructMembers:
            return True
        return False

    #
    # Performs C++ code generation for the feature.
    def generateFeature(self):
        first = True
        for struct in self.featureStructMembers:
            body = '' if first else '\n'
            body += 'struct Decoded_{}\n'.format(struct)
            body += '{\n'
            body += '    using struct_type = {};\n'.format(struct)
            body += '\n'
            body += '    {}* value{{ nullptr }};\n'.format(struct)

            decls = self.makeMemberDeclarations(struct, self.featureStructMembers[struct])
            if decls:
                body += '\n'
                body += decls

            body += '};'

            write(body, file=self.outFile)
            first = False

    #
    # Determines if a Vulkan struct member needs an associated member delcaration in the decoded struct wrapper.
    def needsMemberDeclaration(self, value):
        if value.isPointer or value.isArray:
            return True
        elif self.isFunctionPtr(value.baseType):
            return True
        elif self.isHandle(value.baseType):
            return True
        elif self.isStruct(value.baseType):
            return True
        return False

    #
    # Generate the struct member declarations for the decoded struct wrapper.
    def makeMemberDeclarations(self, name, values):
        body = ''

        for value in values:
            if value.name == 'pNext':
                # We have a special type to store the pNext chain
                body += '    std::unique_ptr<PNextNode> pNext;\n'
            elif self.needsMemberDeclaration(value):
                typeName = self.makeDecodedParamType(value)
                body += '    {} {};\n'.format(typeName, value.name)

        return body
