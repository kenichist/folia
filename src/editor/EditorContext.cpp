#include "editor/EditorContext.h"

#include <QFontDatabase>
#include <QGuiApplication>

namespace folia {

EditorTheme::EditorTheme()
{
    body = QGuiApplication::font();
    body.setPointSizeF(15);
    mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(13);
    using code::Token;
    for (QColor &c : tokens)
        c = text;
    tokens[int(Token::Keyword)] = QColor(173, 61, 164);
    tokens[int(Token::Type)] = QColor(112, 61, 170);
    tokens[int(Token::Builtin)] = QColor(54, 103, 156);
    tokens[int(Token::Function)] = QColor(50, 109, 116);
    tokens[int(Token::String)] = QColor(196, 26, 22);
    tokens[int(Token::Number)] = QColor(28, 0, 207);
    tokens[int(Token::Comment)] = QColor(112, 128, 112);
    tokens[int(Token::Operator)] = QColor(80, 80, 88);
    tokens[int(Token::Punctuation)] = QColor(100, 100, 108);
    tokens[int(Token::Preprocessor)] = QColor(100, 56, 32);
    tokens[int(Token::Variable)] = QColor(15, 104, 160);
    tokens[int(Token::Constant)] = QColor(28, 0, 207);
    tokens[int(Token::Tag)] = QColor(170, 13, 145);
    tokens[int(Token::Attribute)] = QColor(131, 108, 40);
}

EditorTheme EditorTheme::light()
{
    return EditorTheme();
}

EditorTheme EditorTheme::dark()
{
    EditorTheme t;
    t.background = QColor(30, 30, 32);
    t.text = QColor(230, 230, 233);
    t.dim = QColor(140, 140, 148);
    t.accent = QColor(100, 170, 255);
    t.codeBackground = QColor(40, 40, 44);
    t.inlineCodeBackground = QColor(52, 52, 58);
    t.mathSource = QColor(200, 160, 240);
    t.mathError = QColor(255, 110, 100);
    t.linkBackground = QColor(40, 52, 72);
    t.selection = QColor(60, 90, 140);
    using code::Token;
    for (QColor &c : t.tokens)
        c = t.text;
    t.tokens[int(Token::Keyword)] = QColor(252, 95, 163);
    t.tokens[int(Token::Type)] = QColor(208, 168, 255);
    t.tokens[int(Token::Builtin)] = QColor(161, 103, 230);
    t.tokens[int(Token::Function)] = QColor(103, 183, 164);
    t.tokens[int(Token::String)] = QColor(252, 106, 93);
    t.tokens[int(Token::Number)] = QColor(208, 191, 105);
    t.tokens[int(Token::Comment)] = QColor(108, 121, 134);
    t.tokens[int(Token::Operator)] = QColor(200, 200, 205);
    t.tokens[int(Token::Punctuation)] = QColor(180, 180, 186);
    t.tokens[int(Token::Preprocessor)] = QColor(253, 143, 63);
    t.tokens[int(Token::Variable)] = QColor(65, 161, 192);
    t.tokens[int(Token::Constant)] = QColor(208, 191, 105);
    t.tokens[int(Token::Tag)] = QColor(252, 95, 163);
    t.tokens[int(Token::Attribute)] = QColor(191, 133, 85);
    return t;
}

} // namespace folia
