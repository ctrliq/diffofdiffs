/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RENDER_H
#define RENDER_H

#include <review.h>

bool render_section_has_output(const struct review_file *file, int section);

/* A NULL operand name keeps its role visible without a file or revision name */
void render_report(FILE *out, const struct review_report *report,
		   const char *const names[2]);

/*
 * Format-specific entry points take display-ready operand labels. present
 * indexes delta and context; color=false keeps HTML's embedded text report
 * plain regardless of terminal color options.
 */
void render_html_report(FILE *out, const struct review_report *report,
			const char *const identities[2], const bool present[2]);

void render_text_report(FILE *out, const struct review_report *report,
			const char *const identities[2], const bool present[2],
			bool color);

#endif /* RENDER_H */
