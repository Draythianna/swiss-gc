/* -----------------------------------------------------------
      Dolparameters.c - Reads and parses a set of possible 
			argv parameters and values so that they can be 
			displayed to the user later for selection.
	
	- by emu_kidid for Swiss 28/07/2015
   ----------------------------------------------------------- */

#include <argz.h>
#include <envz.h>
#include <gccore.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include "swiss.h"
#include "dolparameters.h"

static Parameters _parameters;

/** 
ParameterValue is used for everything encapsulated by {}'s below.
It is {actual value, user displayed value}.

Example .dcp (DOL Config Parameters) file format:

Name={--videoMode, Video Mode}
Values={pal, PAL 50}, {ntsc, NTSC}

Name={,Interlace}
Values={--nointerlace, No}, {--interlace, Yes}

Name={--turbo, Turbo (default 2)}
Values={1, 1}, {2, 2}, {3, 3}
*/

void printParams(Parameters *params) {
	int i = 0, j = 0;
	print_debug("There are %i parameters\n", params->num_params);
	for(i = 0; i < params->num_params; i++) {
		Parameter *param = &params->parameters[i];
		ParameterValue *arg = &param->arg;
		print_debug("Argument: (%s) [%s]\n", arg->value, arg->name);
		print_debug("This parameter has %i values\n", param->num_values);
		for(j = 0; j < param->num_values; j++) {
			ParameterValue *val = &param->values[j];
			print_debug("Value: (%s) [%s]\n", val->value, val->name);
		}
	}
}

/**
	Given a string with format: "{key,value}"
	this method will return a populated ParameterValue
*/
void parseParameterValue(char *tuple, ParameterValue* val) {
	char* keyStart = NULL;
	char* keyEnd = NULL;
	char* valStart = NULL;
	char* valEnd = NULL;
	
	// Check for a key and for a value
	if((keyStart=strchr(tuple, '{'))!=NULL && (valEnd=strchr(keyStart++, '}'))!=NULL &&
		(keyEnd=memrchr(keyStart, ',', (int)((u32)valEnd-(u32)keyStart)))!=NULL) {
		valStart = keyEnd+1;
		// Trim whitespace
		while(keyStart[0]==' ') keyStart++;
		while(keyEnd[-1]==' ') keyEnd--;
		while(valStart[0]==' ') valStart++;	// avoid whitespace at the start of the value
		while(valEnd[-1]==' ') valEnd--;
		val->value = strndup(keyStart, (int)((u32)keyEnd-(u32)keyStart));
		val->name = strndup(valStart, (int)((u32)valEnd-(u32)valStart));
		//print_debug("Parameter with screen name: [%s] and value [%s]\n",val->name, val->value);
	}
}

/** 
	Given a char array with the contents of a .dcp, 
	this method will allocate and return a populated Parameters struct 
*/
void parseParameters(char *filecontents) {
	char *line = NULL, *linectx = NULL;
	int numParameters = 0;
	line = strtok_r( filecontents, "\r\n", &linectx );

	Parameter *curParam = NULL;	// The current one we're parsing
	while( line != NULL ) {
		//print_debug("Line [%s]\n", line);
		if(line[0] != '#') {
			char *key, *value = NULL;
			key = strtok_r(line, "=", &value);
			
			if(value != NULL) {
				//print_debug("Key [%s] Value [%s]\n", key, value);
			}
			
			if(!strcasecmp("Name", key)) {
				if(value == NULL) continue;
				Parameter *newParams = reallocarray(_parameters.parameters, numParameters+1, sizeof(Parameter));
				if(!newParams) break;
				_parameters.parameters = newParams;
				curParam = &_parameters.parameters[numParameters];
				memset(curParam, 0, sizeof(Parameter));
				//print_debug("Current Param %08X\n", curParam);
				parseParameterValue(value, &curParam->arg);
				if(curParam->arg.value && curParam->arg.name)
					numParameters++;
			}
			else if(!strcasecmp("Values", key)) {
				if(value == NULL || curParam == NULL) continue;
				char *valuePairStart = strchr(value, '{');
				int numValues = 0;
				while(valuePairStart != NULL) {
					ParameterValue *newValues = reallocarray(curParam->values, numValues+1, sizeof(ParameterValue));
					if(!newValues) break;
					curParam->values = newValues;
					memset(&curParam->values[numValues], 0, sizeof(ParameterValue));
					parseParameterValue(valuePairStart, &curParam->values[numValues]);
					valuePairStart = strchr(valuePairStart+1, '{');
					// Only keep tuples that actually parsed.
					if(curParam->values[numValues].value && curParam->values[numValues].name)
						numValues++;
				}
				curParam->num_values = numValues;
			}
		}
		// And round we go again
		line = strtok_r( NULL, "\r\n", &linectx);
	}
	_parameters.num_params = numParameters;
	//printParams(getParameters());
}

Parameters* getParameters() {
	return &_parameters;
}

void populateArgz(char **argz, size_t *argz_len) {
	int i = 0;
	Parameters* params = getParameters();
	print_debug("There are %i parameters\n", params->num_params);
	for(i = 0; i < params->num_params; i++) {
		Parameter *param = &params->parameters[i];
		if(param->enable && param->arg.value && param->num_values > 0) {
			ParameterValue *arg = &param->arg;
			ParameterValue *val = &param->values[param->currentValueIdx];
			if(!val->value) continue;
			//print_debug("Arg: (%s) [%s] is enabled with value (%s) [%s]\n",
			//	arg->value, arg->name, val->value, val->name);
			if(arg->value[0] && val->value[0])
				envz_add(argz, argz_len, arg->value, val->value);
			else if(arg->value[0])
				envz_add(argz, argz_len, arg->value, NULL);
			else if(val->value[0])
				envz_add(argz, argz_len, val->value, NULL);
		}
	}
	print_debug("Arg count: %i\n", argz_count(*argz, *argz_len));
}
