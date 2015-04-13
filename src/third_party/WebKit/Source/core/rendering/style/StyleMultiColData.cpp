/*
 * Copyright (C) 1999 Antti Koivisto (koivisto@kde.org)
 * Copyright (C) 2004, 2005, 2006, 2007, 2008 Apple Inc. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"
#include "core/rendering/style/StyleMultiColData.h"

#include "core/rendering/style/RenderStyle.h"

namespace blink {

StyleMultiColData::StyleMultiColData()
    : m_width(0)
    , m_count(RenderStyle::initialColumnCount())
    , m_columnSpanCount(RenderStyle::initialColumnSpanCount())
    , m_gap(0)
    , m_visitedLinkColumnRuleColor(StyleColor::currentColor())
    , m_autoWidth(true)
    , m_autoCount(true)
    , m_normalGap(true)
    , m_fill(RenderStyle::initialColumnFill())
    , m_spanAllColumns(false)
    , m_breakBefore(RenderStyle::initialPageBreak())
    , m_breakAfter(RenderStyle::initialPageBreak())
    , m_breakInside(RenderStyle::initialPageBreak())
{
}

StyleMultiColData::StyleMultiColData(const StyleMultiColData& o)
    : RefCounted<StyleMultiColData>()
    , m_width(o.m_width)
    , m_count(o.m_count)
    , m_columnSpanCount(o.m_columnSpanCount)
    , m_gap(o.m_gap)
    , m_rule(o.m_rule)
    , m_visitedLinkColumnRuleColor(o.m_visitedLinkColumnRuleColor)
    , m_autoWidth(o.m_autoWidth)
    , m_autoCount(o.m_autoCount)
    , m_normalGap(o.m_normalGap)
    , m_fill(o.m_fill)
    , m_spanAllColumns(o.m_spanAllColumns)
    , m_breakBefore(o.m_breakBefore)
    , m_breakAfter(o.m_breakAfter)
    , m_breakInside(o.m_breakInside)
{
}

bool StyleMultiColData::operator==(const StyleMultiColData& o) const
{
    return m_width == o.m_width && m_count == o.m_count && m_columnSpanCount == o.m_columnSpanCount && m_gap == o.m_gap
        && m_rule == o.m_rule && m_visitedLinkColumnRuleColor == o.m_visitedLinkColumnRuleColor && m_breakBefore == o.m_breakBefore
        && m_autoWidth == o.m_autoWidth && m_autoCount == o.m_autoCount && m_normalGap == o.m_normalGap
        && m_fill == o.m_fill && m_spanAllColumns == o.m_spanAllColumns
        && m_breakAfter == o.m_breakAfter && m_breakInside == o.m_breakInside;
}

} // namespace blink
