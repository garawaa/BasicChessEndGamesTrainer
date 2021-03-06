/*
 * Basic Chess Endgames : generates a position of the endgame you want, then play it against computer.
    Copyright (C) 2017-2018  Laurent Bernabe <laurent.bernabe@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
package com.loloof64.android.basicchessendgamestrainer.position_generator_editor.script_language

import com.loloof64.android.basicchessendgamestrainer.MyApplication
import com.loloof64.android.basicchessendgamestrainer.R
import com.loloof64.android.basicchessendgamestrainer.position_generator_editor.script_language.antlr4.ScriptLanguageLexer
import org.antlr.v4.runtime.CharStream
import org.antlr.v4.runtime.LexerNoViableAltException
import org.antlr.v4.runtime.misc.Interval
import org.antlr.v4.runtime.misc.ParseCancellationException

class BailScriptLanguageLexer(val input: CharStream) : ScriptLanguageLexer(input) {
    override fun recover(error: LexerNoViableAltException?) {
        val lowerBound = error?.startIndex!!
        val offendingText = input.getText(Interval(lowerBound, lowerBound))
        val messageFormat = MyApplication.appContext.resources.getString(R.string.parser_unrecognized_symbol)
        val message = String.format(messageFormat, offendingText)
        throw ParseCancellationException(message)
    }
}