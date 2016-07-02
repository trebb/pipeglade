<?xml version="1.0" encoding="UTF-8" ?>
<xsl:stylesheet version="1.0"
                xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <!-- Extract the hierarchy of section headings from html -->
  <xsl:output method="text"/>

  <xsl:template match="script|noscript"/>

  <xsl:template match="@*|node()">
    <xsl:apply-templates/>
  </xsl:template>

  <xsl:template match="div[@class='section']/h1">
    <xsl:value-of select="."/>
    <xsl:text>&#xA;</xsl:text>
  </xsl:template>

  <xsl:template match="div[@class='subsection']/h2">
    <xsl:text>    </xsl:text>
    <xsl:value-of select="."/>
    <xsl:text>&#xA;</xsl:text>
  </xsl:template>

  <xsl:template match="div[@class='subsection']/dl/dt/text()">
    <xsl:text>      </xsl:text>
    <xsl:choose>
      <xsl:when test="starts-with(., '&#xA;')">
        <xsl:value-of select="substring-after(., '&#xA;')"/>
      </xsl:when>
      <xsl:otherwise>
        <xsl:value-of select="."/>
      </xsl:otherwise>
    </xsl:choose>
    <xsl:text>&#xA;</xsl:text>
  </xsl:template>

  <xsl:template
      match="div[@class='subsection']/dl/dd/b[@class='flag']">
    <xsl:if test="starts-with(., ':')">
      <xsl:if test="string-length(.)>2">
        <xsl:text>        </xsl:text>
        <xsl:value-of select="."/>
        <xsl:text>&#xA;</xsl:text>
      </xsl:if>
    </xsl:if>
  </xsl:template>

</xsl:stylesheet>
