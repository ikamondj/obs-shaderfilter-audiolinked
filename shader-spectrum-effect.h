#pragma once

#include <graphics/graphics.h>
#include <util/cf-parser.h>
#include <util/dstr.h>
#include "shader-helpers.generated.h"

/* OBS resolves function dependencies by name rather than signature. Route
 * two-argument calls to a distinct internal function after preprocessing so
 * macros, nested expressions and both call forms work on all backends. */
static inline bool sf_shader_spectrum_has_falloff(struct cf_token *token)
{
	if (token->type != CFTOKEN_NAME || strref_cmp(&token->str, "spectrum") != 0)
		return false;
	do {
		++token;
	} while (token->type == CFTOKEN_SPACETAB || token->type == CFTOKEN_NEWLINE);
	if (strref_cmp(&token->str, "(") != 0)
		return false;
	int depth = 1;
	int commas = 0;
	while ((++token)->type != CFTOKEN_NONE) {
		if (strref_cmp(&token->str, "(") == 0 || strref_cmp(&token->str, "[") == 0 || strref_cmp(&token->str, "{") == 0) {
			++depth;
		} else if (strref_cmp(&token->str, ")") == 0 || strref_cmp(&token->str, "]") == 0 ||
			   strref_cmp(&token->str, "}") == 0) {
			if (--depth == 0)
				return commas == 1;
		} else if (depth == 1 && strref_cmp(&token->str, ",") == 0) {
			++commas;
		}
	}
	return false;
}

/* Match an active top-level declaration after OBS preprocessing. Comments,
 * strings, disabled #ifdef branches, unrelated names and texture arrays do not
 * enable the helper. Runtime opt-in is separately checked by effect reflection. */
static inline bool sf_shader_prepare_spectrum(const char *text, struct dstr *expanded)
{
	struct cf_parser parser;
	cf_parser_init(&parser);
	// Match OBS's backend defines using its public graphics API.
	const int device = gs_get_device_type();
	const char *backend = device == GS_DEVICE_OPENGL ? "_OPENGL" : device == GS_DEVICE_DIRECT3D_11 ? "_D3D11" : "_Metal";
	if (backend) {
		struct cf_def def;
		cf_def_init(&def);
		def.name.str.array = backend;
		def.name.str.len = strlen(backend);
		strref_copy(&def.name.unmerged_str, &def.name.str);
		cf_preprocessor_add_def(&parser.pp, &def);
	}
	struct cf_token *declaration_end = NULL;
	if (cf_parser_parse(&parser, text, NULL) && !error_data_has_errors(&parser.error_list)) {
		int depth = 0;
		int matched = 0;
		const char *declaration[] = {"uniform", "texture2d", "spec"};
		for (struct cf_token *token = parser.cur_token; token && token->type != CFTOKEN_NONE; ++token) {
			if (token->type == CFTOKEN_SPACETAB || token->type == CFTOKEN_NEWLINE)
				continue;
			if (strref_cmp(&token->str, "{") == 0)
				++depth;
			if (strref_cmp(&token->str, "}") == 0)
				--depth;
			if (!depth && matched == 3 && (strref_cmp(&token->str, ";") == 0 || strref_cmp(&token->str, "<") == 0)) {
				if (strref_cmp(&token->str, "<") == 0) {
					while (token->type != CFTOKEN_NONE && strref_cmp(&token->str, ">") != 0)
						++token;
					if (token->type != CFTOKEN_NONE)
						++token;
					while (token->type == CFTOKEN_SPACETAB || token->type == CFTOKEN_NEWLINE)
						++token;
				}
				if (strref_cmp(&token->str, ";") == 0)
					declaration_end = token;
				break;
			}
			if (!depth && matched < 3 && strref_cmp(&token->str, declaration[matched]) == 0)
				++matched;
			else
				matched = !depth && strref_cmp(&token->str, "uniform") == 0 ? 1 : 0;
		}
	}
	const bool found = declaration_end != NULL;
	if (found && expanded) {
		/* OBS records dependencies as it parses function bodies: the helper
		 * must follow spec's declaration and precede callers. Use expanded
		 * tokens so macro-generated declarations and conditional branches
		 * behave exactly like the effect compiler's own preprocessing. */
		for (struct cf_token *token = parser.cur_token; token->type != CFTOKEN_NONE; ++token) {
			if (sf_shader_spectrum_has_falloff(token))
				dstr_cat(expanded, "sf_spectrum_sample");
			else
				dstr_cat_strref(expanded, &token->str);
			if (token == declaration_end) {
				dstr_cat(expanded, "\n");
				dstr_cat(expanded, effect_template_spectrum);
			}
		}
	}
	cf_parser_free(&parser);
	return found;
}

static inline bool sf_shader_declares_spec(const char *text)
{
	return sf_shader_prepare_spectrum(text, NULL);
}

static inline bool sf_shader_append_spectrum(struct dstr *text)
{
	struct dstr expanded = {0};
	const bool found = sf_shader_prepare_spectrum(text->array, &expanded);
	if (found)
		dstr_copy_dstr(text, &expanded);
	dstr_free(&expanded);
	return found;
}
